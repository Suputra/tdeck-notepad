// --- Command Processor ---

// --- Streaming Upload/Download ---

static volatile bool upload_running = false;
static volatile int upload_done_count = 0;
static volatile int upload_total_count = 0;
static volatile bool download_running = false;
static volatile int download_done_count = 0;
static volatile int download_total_count = 0;
static volatile uint32_t upload_bytes_done = 0;
static volatile uint32_t upload_bytes_total = 0;
static volatile uint32_t download_bytes_done = 0;
static volatile uint32_t download_bytes_total = 0;
static volatile uint32_t upload_started_ms = 0;
static volatile uint32_t download_started_ms = 0;
static volatile uint32_t upload_last_ui_ms = 0;
static volatile uint32_t download_last_ui_ms = 0;

static constexpr size_t TRANSFER_CHUNK_SIZE = 256;
static constexpr size_t TRANSFER_LINE_MAX = 192;

uint32_t satAddU32(uint32_t a, uint32_t b) {
    if (UINT32_MAX - a < b) return UINT32_MAX;
    return a + b;
}

void formatBytesCompact(uint32_t bytes, char* out, size_t out_len) {
    if (!out || out_len < 2) return;
    if (bytes < 1024) {
        snprintf(out, out_len, "%luB", (unsigned long)bytes);
        return;
    }
    if (bytes < 1024UL * 1024UL) {
        unsigned long whole = bytes / 1024UL;
        unsigned long frac = (bytes % 1024UL) * 10UL / 1024UL;
        snprintf(out, out_len, "%lu.%luK", whole, frac);
        return;
    }
    unsigned long whole = bytes / (1024UL * 1024UL);
    unsigned long frac = (bytes % (1024UL * 1024UL)) * 10UL / (1024UL * 1024UL);
    snprintf(out, out_len, "%lu.%luM", whole, frac);
}

void maybeTransferUiRefresh(volatile uint32_t* last_ms) {
    uint32_t now = millis();
    if (now - *last_ms >= 250) {
        *last_ms = now;
        render_requested = true;
    }
}

bool isSafeTransferName(const char* name) {
    if (!name || name[0] == '\0') return false;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
    for (const char* p = name; *p; p++) {
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') ||
                  (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

bool sshWriteAll(ssh_channel channel, const uint8_t* data, size_t len) {
    size_t offset = 0;
    while (offset < len) {
        int n = ssh_channel_write(channel, data + offset, len - offset);
        if (n <= 0) return false;
        offset += (size_t)n;
    }
    return true;
}

bool sshReadExact(ssh_channel channel, uint8_t* out, size_t len) {
    size_t offset = 0;
    while (offset < len) {
        int n = ssh_channel_read(channel, out + offset, len - offset, 0);
        if (n <= 0) return false;
        offset += (size_t)n;
    }
    return true;
}

bool sshReadLine(ssh_channel channel, char* out, size_t out_len) {
    if (!out || out_len < 2) return false;
    size_t pos = 0;
    while (true) {
        char c = 0;
        int n = ssh_channel_read(channel, &c, 1, 0);
        if (n <= 0) return false;
        if (c == '\r') continue;
        if (c == '\n') {
            out[pos] = '\0';
            return true;
        }
        if (pos + 1 >= out_len) return false;
        out[pos++] = c;
    }
}

bool sshOpenExecChannel(const char* command, ssh_channel* out_channel) {
    if (!ssh_connected || !ssh_sess || !command || !out_channel) return false;
    ssh_channel channel = ssh_channel_new(ssh_sess);
    if (!channel) return false;
    if (ssh_channel_open_session(channel) != SSH_OK) {
        ssh_channel_free(channel);
        return false;
    }
    if (ssh_channel_request_exec(channel, command) != SSH_OK) {
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        return false;
    }
    *out_channel = channel;
    return true;
}

int sshCloseExecChannel(ssh_channel channel) {
    if (!channel) return -1;
    ssh_channel_send_eof(channel);
    ssh_channel_close(channel);
    int exit_status = ssh_channel_get_exit_status(channel);
    ssh_channel_free(channel);
    return exit_status;
}

bool uploadStreamFile(ssh_channel channel, const char* file_name) {
    if (!isSafeTransferName(file_name)) return false;

    String path = "/" + String(file_name);
    sdAcquire();
    File f = SD.open(path.c_str(), FILE_READ);
    if (!f) {
        sdRelease();
        return false;
    }
    size_t sz = f.size();

    char header[TRANSFER_LINE_MAX];
    int hdr_len = snprintf(header, sizeof(header), "FILE %lu %s\n", (unsigned long)sz, file_name);
    if (hdr_len <= 0 || hdr_len >= (int)sizeof(header)) {
        f.close();
        sdRelease();
        return false;
    }
    if (!sshWriteAll(channel, (const uint8_t*)header, (size_t)hdr_len)) {
        f.close();
        sdRelease();
        return false;
    }

    uint8_t buf[TRANSFER_CHUNK_SIZE];
    while (true) {
        int n = f.read(buf, sizeof(buf));
        if (n < 0) {
            f.close();
            sdRelease();
            return false;
        }
        if (n == 0) break;
        if (!sshWriteAll(channel, buf, (size_t)n)) {
            f.close();
            sdRelease();
            return false;
        }
        upload_bytes_done = satAddU32(upload_bytes_done, (uint32_t)n);
        maybeTransferUiRefresh(&upload_last_ui_ms);
    }

    f.close();
    sdRelease();

    const char nl = '\n';
    return sshWriteAll(channel, (const uint8_t*)&nl, 1);
}

bool parseDownloadHeader(const char* line, size_t* out_size, char* out_name, size_t out_name_len) {
    if (!line || !out_size || !out_name || out_name_len < 2) return false;
    if (strncmp(line, "FILE ", 5) != 0) return false;

    const char* p = line + 5;
    char* end = NULL;
    unsigned long sz = strtoul(p, &end, 10);
    if (!end || end == p || *end != ' ') return false;

    const char* name = end + 1;
    if (!isSafeTransferName(name)) return false;

    strncpy(out_name, name, out_name_len - 1);
    out_name[out_name_len - 1] = '\0';
    *out_size = (size_t)sz;
    return true;
}

bool downloadStreamFile(ssh_channel channel, const char* file_name, size_t file_size) {
    String path = "/" + String(file_name);
    uint8_t buf[TRANSFER_CHUNK_SIZE];
    size_t remaining = file_size;

    sdAcquire();
    SD.remove(path.c_str());
    File f = SD.open(path.c_str(), FILE_WRITE);
    bool file_ok = (bool)f;

    while (remaining > 0) {
        size_t chunk = remaining > sizeof(buf) ? sizeof(buf) : remaining;
        if (!sshReadExact(channel, buf, chunk)) {
            if (f) f.close();
            sdRelease();
            return false;
        }
        if (file_ok) {
            size_t w = f.write(buf, chunk);
            if (w != chunk) file_ok = false;
        }
        download_bytes_done = satAddU32(download_bytes_done, (uint32_t)chunk);
        maybeTransferUiRefresh(&download_last_ui_ms);
        remaining -= chunk;
    }
    if (f) f.close();
    sdRelease();

    char sep = 0;
    if (!sshReadExact(channel, (uint8_t*)&sep, 1)) return false;
    if (sep != '\n') return false;
    return file_ok;
}

static const char* REMOTE_UPLOAD_STREAM_CMD =
    "/bin/sh -c '"
    "set -eu; "
    "dest=\"$HOME/tdeck\"; "
    "mkdir -p \"$dest\"; "
    "while IFS= read -r header; do "
    "  [ \"$header\" = \"DONE\" ] && break; "
    "  case \"$header\" in FILE\\ *) ;; *) exit 31;; esac; "
    "  meta=${header#FILE }; "
    "  len=${meta%% *}; "
    "  name=${meta#* }; "
    "  case \"$len\" in \"\"|*[!0-9]*) exit 32;; esac; "
    "  case \"$name\" in \"\"|*/*) exit 33;; esac; "
    "  tmp=\"$dest/.${name}.tmp.$$\"; "
    "  dd bs=1 count=\"$len\" of=\"$tmp\" 2>/dev/null || exit 34; "
    "  IFS= read -r _sep || exit 35; "
    "  mv \"$tmp\" \"$dest/$name\" || exit 36; "
    "done; "
    "printf \"OK\\n\""
    "'";

static const char* REMOTE_DOWNLOAD_STREAM_CMD =
    "/bin/sh -c '"
    "set -eu; "
    "src=\"$HOME/tdeck\"; "
    "[ -d \"$src\" ] || { printf \"DONE\\n\"; exit 0; }; "
    "for f in \"$src\"/*; do "
    "  [ -f \"$f\" ] || continue; "
    "  name=${f##*/}; "
    "  case \"$name\" in \"\"|*/*) continue;; esac; "
    "  size=$(wc -c < \"$f\" | tr -d \"[:space:]\"); "
    "  printf \"FILE %s %s\\n\" \"$size\" \"$name\"; "
    "  cat \"$f\"; "
    "  printf \"\\n\"; "
    "done; "
    "printf \"DONE\\n\""
    "'";

void uploadTask(void* param) {
    upload_running = true;
    upload_done_count = 0;
    upload_bytes_done = 0;
    upload_bytes_total = 0;
    upload_started_ms = millis();
    upload_last_ui_ms = upload_started_ms;

    // List flat files at root
    int n = listDirectory("/");
    upload_total_count = 0;
    for (int i = 0; i < n; i++) {
        if (!file_list[i].is_dir && isSafeTransferName(file_list[i].name)) {
            upload_total_count++;
            uint32_t fsz = file_list[i].size > UINT32_MAX ? UINT32_MAX : (uint32_t)file_list[i].size;
            upload_bytes_total = satAddU32(upload_bytes_total, fsz);
        }
    }
    cmdSetResult("Uploading %d files...", upload_total_count);
    render_requested = true;

    if (upload_total_count == 0) {
        cmdSetResult("No valid files to upload");
        render_requested = true;
        upload_running = false;
        vTaskDelete(NULL);
        return;
    }

    ssh_channel channel = NULL;
    if (!sshOpenExecChannel(REMOTE_UPLOAD_STREAM_CMD, &channel)) {
        cmdSetResult("Upload start failed: %s", ssh_get_error(ssh_sess));
        render_requested = true;
        upload_running = false;
        vTaskDelete(NULL);
        return;
    }

    bool stream_ok = true;
    for (int i = 0; i < n; i++) {
        if (file_list[i].is_dir) continue;
        if (!isSafeTransferName(file_list[i].name)) continue;
        if (!uploadStreamFile(channel, file_list[i].name)) {
            stream_ok = false;
            break;
        }
        upload_done_count++;
        render_requested = true;
    }

    if (stream_ok) {
        const char done[] = "DONE\n";
        stream_ok = sshWriteAll(channel, (const uint8_t*)done, sizeof(done) - 1);
    }
    ssh_channel_send_eof(channel);

    char reply[TRANSFER_LINE_MAX];
    bool got_reply = stream_ok && sshReadLine(channel, reply, sizeof(reply));
    int exit_status = sshCloseExecChannel(channel);

    if (stream_ok && got_reply && strcmp(reply, "OK") == 0 && exit_status == 0) {
        cmdSetResult("Upload done: %d files", upload_done_count);
    } else {
        cmdSetResult("Upload failed (%d/%d)", upload_done_count, upload_total_count);
    }
    render_requested = true;
    upload_running = false;
    vTaskDelete(NULL);
}

void downloadTask(void* param) {
    download_running = true;
    download_done_count = 0;
    download_total_count = 0;
    download_bytes_done = 0;
    download_bytes_total = 0;
    download_started_ms = millis();
    download_last_ui_ms = download_started_ms;

    ssh_channel channel = NULL;
    if (!sshOpenExecChannel(REMOTE_DOWNLOAD_STREAM_CMD, &channel)) {
        cmdSetResult("Download start failed: %s", ssh_get_error(ssh_sess));
        render_requested = true;
        download_running = false;
        vTaskDelete(NULL);
        return;
    }

    cmdSetResult("Downloading...");
    render_requested = true;

    bool stream_ok = true;
    bool saw_done = false;
    while (stream_ok) {
        char line[TRANSFER_LINE_MAX];
        if (!sshReadLine(channel, line, sizeof(line))) {
            stream_ok = false;
            break;
        }
        if (strcmp(line, "DONE") == 0) {
            saw_done = true;
            break;
        }

        size_t file_size = 0;
        char file_name[64];
        if (!parseDownloadHeader(line, &file_size, file_name, sizeof(file_name))) {
            stream_ok = false;
            break;
        }

        download_total_count++;
        uint32_t fsz = file_size > UINT32_MAX ? UINT32_MAX : (uint32_t)file_size;
        download_bytes_total = satAddU32(download_bytes_total, fsz);
        if (downloadStreamFile(channel, file_name, file_size)) {
            download_done_count++;
            render_requested = true;
        } else {
            stream_ok = false;
        }
    }

    int exit_status = sshCloseExecChannel(channel);

    if (stream_ok && saw_done && exit_status == 0) {
        cmdSetResult("Download done: %d files", download_done_count);
    } else {
        cmdSetResult("Download failed (%d/%d)", download_done_count, download_total_count);
    }
    render_requested = true;
    download_running = false;
    vTaskDelete(NULL);
}

// --- Power Off Art ---

static const char poweroff_art[] PROGMEM =
    "                    .\n"
    "                    .ll,.\n"
    "                     ,,'.\n"
    "                        ..\n"
    "                         ..\n"
    "                          ..            ...',;;;;,..\n"
    "                           ..       ..',;:::;;:;;:,'.\n"
    "                            ..   .',;:;;;;::;::::,'',.\n"
    "                             .,''::;;;;;;;;;;:::,'',',','.\n"
    "                          .';;;,,',;;;;;::::::;,',,,,',,.\n"
    "                        .,cc:;;,''';:::ccclc:;;,,'',,'',,.\n"
    "                      .;cccccc:;;:ccllllolc::;;;;,'',''',,'..\n"
    "                    .;cc:cccllclllllllollc:::::;;;,'',',,,'''.\n"
    "                   ,lccclllllllllloooolc::::::::;;;,,,',,'.'''........\n"
    "                 .:lccllllllllllooodoooc:::::::::;;;;,,,'.''',;::;,,,,,'.\n"
    "                .locclllllooooodxxxddddoc:::::::::;;;;,'''',:::;,,''''''.\n"
    "                .dxllloodxkO000OOOOxddddol::::::::::;,,,'''cxkko;''',,''''.\n"
    "                 'x0OO00KXNWWWWX0OOOkddddol:::::::;;;;;:;,,lkkko,''',;,'''.\n"
    "                  ,ONNXXNWWWWWWWN0OOOkdoodol:::::::::;;;:,'cxk0x:'''''''',.\n"
    "                   .kNNXXNWWWWWWWN0OOOkdooddl::::c::::;;,,';dkOkc,,''','',.\n"
    "                    .dNNXXNWWWWWWWN0OOOkxoollccc:::::::;,,'.;lllc;',,,,,,.\n"
    "                     .lXNXXNWWNWWWWNKOkxdlclodolc:::::::;;,,'';:::;,,''..\n"
    "                     .,lKNXXNWWWNXK0kdlodxxxxxxolc::::::::;;'..'.....\n"
    "                    .lo,cxkxxkkxxdddxOO0KK0kxxxxolcc::::::,....'.\n"
    "                    .okclxkkkOO0KKXNNWNXOddxxxxxxdlcc:;;,'.',,'..         .. .....\n"
    "                     :0xlxKWWNNXXNWWWWNx,,,,lxxxxxdoc;'..',,,,,....     ..        ..\n"
    "                     .l0kod0XkccclONWWNx,::,lOxxxdl:,'';;:::;'......  .            ..    ..\n"
    "                       ;xkdoxdccc,cXWNWXOddk0Odc;,',:clllcc;.. .......          ....'''',,''.\n"
    "                        .';,.:dxdd0WWWNX0Oxol:::clddxdool:'.....  ..... .....',,,,,,,,,;,''''.\n"
    "                               .cllllxkdoodxO000Okkkkxo:'........   .....''',,,,,,,,,;;,''''''.\n"
    "                                     .xXXKXNWWXXX0kdc,'....'......  . .......,,,,,,,:;''.....''..''...\n"
    "                                      .l0KKKXKOxo:'. .''''',;,'..............,,;;;;c;'.....'::,,,,,,,,.\n"
    "                                        .'''...     .::;;:c:;;,'...........';:::::c;'.....;ol,,,''''',,'.\n"
    "                                                  .cxxoc;:c:;::,.........';ccllc:c;'..',':kOl,,''','''',.\n"
    "                                                 ;xkddccc:cc:;;;,,,'..',;codddocc;''.';;,:ddc,,'',;,..'''.\n"
    "                                               .:kkxxx::c:lcccc:;,;::::lodddddll:,,'..''.,xOo;,,'','..','\n"
    "                                              .oOOkkkx:,llc:;:clooooooddddxxollc,,,,......o0x:,,,''''',,.\n"
    "                                             .d00OOOxOkdodddddoddddxkkkkkkkxocc;,,,,'......;:;,'',,,,,'.\n"
    "                                            .dkOK000OOOOOkkOkxxxkOO00KKK000dcc;,,,,,''........',,'''....\n"
    "                                       .;:;:xKKKK00000000000KKKXXXXNNXXKKK0occ;,,,,,,''...... .:l:;'''''.\n"
    "                                     .cxOOokKXNXKKKXXXXNNNNNXXKK00000OOOkO0x:::,,,,,,,'''......cOxoc,'''.\n"
    "                                     :kOXkdXXWWNNXXKK000000000000000KXKK0Okkl::;,,,,,,,','''''.,kOdl;'''.\n"
    "                                     o0ONxxXK0KKK000000KKXXXXNNNXXX0KXXKKOkOxc::;,,,,,,,,,''''''o0xo:,''.\n"
    "                                     l0OXOd0NNNNNNNWNNNXXK0Okxxdoolc:ckXX0Okko:::,,,,,,,,,,,,,,';kOdo;'''.\n"
    "                                     'kOOXxxNWNK0OOkxdooc:;,,,,,,,;,,':OXKOkkxc;:;,',',,,,,,,,,,'l0kdc,''.\n"
    "                          ,,          'dxxkdOW0ollllol;;;;;;;;;,,,,,,,'lKXOkxkd:::;,',,,,,,,,,,,';O0xo;''..\n"
    "                         'ol         .cdo:lloK0ddc:::ol;,,;,,',;,;;;;,,,dXKOxxxl;::;,,,,,,,,,,,,''oKOd:.....\n"
    "                          ,'       .:OKOd:...cdccdko;;:;,,;lolll;;;,;;,,:kKOkxkxc:::,,,,,,,,,,,,,',loc;,'....\n"
    "                          ,'      ,xKOxd:.   .oxoodoc;,,,',cdc;::,;,,;,,'cOOkkxkd:::;,,,,,,,,,,,,,''oOdl:'..\n"
    "                          ,     .oK0xo:.   .'dKOl;;lxxc;,,,';c:ldldo:;,,,,oOkkkxxl;::;',,,,,,,,,,,,'cOkoc,...\n"
    "                    ,:cllclc:::;lxdl:..  .:do::cooc:ddc;,,,,,;;;::::;;,',';xOkkxxdc;::,,,,,,,,,,,,,'cOxl:,''.\n"
    "                  ckKNWWNNNXXXXK0ko:,.  .cxo,..ckkkd:;cl,',,,'',,,;::;,',,'ckOxdddo:::;,,,,,,,,,,,,,'cOxl:,''.\n"
    "                 c0KKKKKKKKKKKK0Odlc:'   ......;xkl:lcoxc,,',,',;''ldlllllclddollllc::;,''',,,,,,,',.,xkdl;''.\n"
    "                 dNWNXNNNNNNXXNN0olc;c;..:::cc,..;okOocol;,','.....,::ccccllllllc:cllccc:::;;:;;;,,'..coc;'.....\n"
    "               xxkNKl:xXNNKl:oOK0oc:lxl,,,.....'oxlc:,..,,,,''.....',,,,,,;;;;;::,;::::::cccclccc::;,;,',,,,,,,'''.\n"
    "                :xNKocxXNNKo:dOKOlc;;c,.     .cdo:.'ll,.:x:''.....',,,,,''''',,,''',,,,,,,,;;;;;;;;;,,,..cl;,,,,,','.\n"
    "                 oNWNKKKK0000XXXklc:,;'   .;ll:,.  .d0d::l;,,,:lolc:;;,,,'',,;,,'',,,,,,,,,,,',,',,,'''.:l,,,,,,'''',.\n"
    "                 lNWXOkxxxxdkKXXklccc:,  .:l:'.     .xNOxdodk0KOxxol:,,;clcc::;,,,,,,,;;;;,,,,,,,,,'.'.'lc,,,,:,.',','\n"
    "                 'coddddxxddddooc;,;;     ..         .kWXXXXXXKoldcol,'':oxxxddollc:;,,,''''',,;,,''''.'lc,,'''..''',.\n"
    "                       ,:;,''.                        'ONXXXXXKklol:cl;';ldddddddddolc;;;,''......  .....,;,,,,,','.\n"
    "                       cxoc:;'                         ,KNXXXXK0kolcclcclodddddddddolc;;;,''......  .................\n"
    "               .'cdkOklokocc:,,,'',;::;,.               ,k0KXXKKK00Okkxxddddddoooolc:;,''... .....        ........\n"
    "        ..;clc;lOXXXXKxddoccclllddxdll:;;,'...            ..'''''''''..''''...........    ........\n"
    "        ;ONWNkx0KKKKKKKKK000000KKK0d:;,:kXK0kl.                        .'.........................\n"
    "        'oKN0dkWWK000KKKKK0K000XXK0o;;'cOK0Ol'.                        .',;:::;,''...... .........\n"
    "        .'lkxlkWXdcc::::ccllclokKK0o;,':kkkl'..                 .....  .,;;:cc:;'.....................\n"
    "      ..lxc;;,dNXdc:codoccc::ccxKK0o;,.':c:',:;'             .cxO0Oo::c;,;::lll:,................''''.\n"
    "     .;k0d:. .oNKdc:lkkxolo:;ccxKK0o:;,,'.  'odc,.          ,kKXX0olxxl;'.,::::;,................'''''.\n"
    "    .cKKx:.   oNKd:;cdlc::c;:ccxKK0o::;:'    .dOo;.        ,OKXNXol0X0kxdlloooool:;;,,',;:cc:,'......',,,,,,......\n"
    "   .,dxo;.   .oXKd:;:dkxxxdolc:d00Oo::;:'     .ox:'.      .dKKXXO:l00000OOOOOOOOOOOkxxxxxkxl;'.....'cdddol:,,,,,,,'.\n"
    "   ,lc,.     .oXKdc::lxkkdlcc:;oOOOo::;:'       ',,,.     'O0O00x;l0000Okkkkkkkkkxdddddddo:,,..,;':xOOkkx:,,,,,,,,,,.\n"
    "  .d0d;.     .lKKkolloxkkdlllllxOOko::;:.       .:dl;     ,0K0O0d,;xOkkkkkkkkkxxxxdddddddo;,...,.:OOOkxx:',,'..'',,,'\n"
    "  ,KKx,       cO000OOOOOkkkkkkkkkOko:;,,.        cKxc.    ;0KO00d,.;dxddxxxxdddddolooooooc;'.....dOxxxkl',,'.''..',,'.\n"
    "  cX0o.       ,xkkkkkkkkkxxxxxxxxxxl;,,,.        'OOo;   .xNXKKx:,'.,ldooooooollllcllclllc,'....'xxoodd:',,.';,..',,.\n"
    "  ,ol;.       .xOkOOOOkkkkkkkkkkkxxl::;.         .co:,. .dXXXXk:,,''..,:ccccccccccc:::::::,'... ,kxdddl:',,'.''..','.\n"
    ".:dxdc.        ':ccc::::ccccccccccc;'..          .;:,,'.lXXXX0c,;,'','.......'',,,,;;;;,;;;,,'..dOxdxd:'',,,'.''',,.\n"
    "lX0xkOd,         ;:.'.         ...''.           :kxodxl;cOXNKl,;;;,,'.               ..........dOxddd:,'',,,,,,'.\n"
    "'OKc..ckc.       'kd:;.          :o:;.          'Ox..'lo,'lOOo;;;;;;,.                        .oOxdddc','',,,,,,'.\n"
    ".cx:. 'c.        l0dl,           l0dl,          .c;  .:c',ll:,,;,,;,.                        .lOxdddl:',,,'''...\n"
    " .             .kOoc.           :Oxl;.              .;clkkoc;,,',,.                         ckdoddl;,,,,,,'.\n"
    "             ..:xoc,            ;xo:,.             .dKKXKxo:,''''.                         .clc:cc:',,,''.\n"
    "            'xKK0Oo;..         ,dkxdl;..          .xXXX0xo:,,;,..                          ,oddoc,''',','.\n"
    ".            .o0KXX0o;'.       .:dKXX0x;'.       ...ckO0koo;',;,.                           .okkkdc,''''''.\n"
    ";.           'xKXXXOl:;'       'cxKKK0xc,'.    .,oo:;clc;,,',;,..                          .okkkdc,','..'....        .........\n"
    "::dxkkOOOOkOdoOKXXKkc:;:dOOOOOkocd0000kl,';dOOkodo;',;;;;cc:;;,;cllllllccc:::clllllllcccc::dkkxdc:',,,'..',;;;;,,,;;;;;;;;''''\n"
    "l:coodkXWWWWOd0KKKKxc:;:0WWWWWXdcdO0OOko;,,oXWKdlokkkkxdooc:;;.';::;coodooooddxolllc:cc:;;okxxxc:',,,''.';;;;;,,,,,;:;;;:;''..\n"
    "l::dxk0NWWWNkx0KKKKd::;:OWWWWWXxcdOOOOkdc,,:ONKd;,:cloooc;;;:;,;:;'';:cc::::;;;,,,,,,,,,,,;::c:,'''..'..''''..','''''''''.',;,\n"
    "ddx0K00OkKNKxkKKKK0d::;c0WWWWWXxcdOOOOkxl;;,oXNKdlc:,''''.',;;;,'........'.''''','''.';lc'',;;'''..,;;,,,,,,,...........,:cc::\n"
    "NKkddxOOO00xdOKKKKOl;:;:xWWWWNKdcokOOkkxo:,.;ONWWNNKkollllc:,'',,;::::::ccccccc::cllc:lo:,,,,,'';;:;,;:cc:;;,'.........,cc,;;,\n"
    "OOxooddokNNxd00OkOkl;,,:OWWWWXkolokOkxxdoc,';ONNNNXXXX0kkkkkkkkOOOxddkOOkkkkxxxoodddo::::llccc::;,;;;;;;,,,,,,',,'.....,:,,:;,\n"
    "O0OxddxxONNdcodddddc,'.,ldxkOkdooldkdoddo:,.':cccclllloooololloool:;:cllc::cc::::;,,,'...''''',,'.',,,''',,,,,:;,,,'....,;,,,,\n"
    "WNWWWWWWWWWk;,'''.........',,;;;;,,,,,,,'............'''''',,,;::,''''.'''',,'''''''''...'''.':::;'.......'',:c,,:cc::;;,,,,,,\n";

#define POWEROFF_ART_LINES 92
#define POWEROFF_ART_PX_W  2
#define POWEROFF_ART_PX_H  3

struct PoweroffBlockGlyph {
    char ch;
    uint8_t shape_bits;
    uint8_t shade;
};

static constexpr PoweroffBlockGlyph kPoweroffBlockGlyphs[] = {
    { ' ', 0b000000, 0 },
    { '\'', 0b110000, 3 },
    { ',', 0b000011, 3 },
    { '.', 0b000001, 3 },
    { ':', 0b010001, 3 },
    { ';', 0b010011, 3 },
    { 'c', 0b111111, 1 },
    { 'd', 0b111111, 2 },
    { 'k', 0b111111, 2 },
    { 'l', 0b111111, 1 },
    { 'o', 0b111111, 2 },
    { 'x', 0b111111, 2 },
    { '0', 0b111111, 3 },
    { 'K', 0b111111, 3 },
    { 'N', 0b111111, 3 },
    { 'O', 0b111111, 3 },
    { 'W', 0b111111, 3 },
    { 'X', 0b111111, 3 },
};

inline const PoweroffBlockGlyph& poweroffBlockGlyphFor(char ch) {
    static constexpr PoweroffBlockGlyph fallback = { '?', 0b111111, 2 };
    for (const auto& glyph : kPoweroffBlockGlyphs) {
        if (glyph.ch == ch) return glyph;
    }
    return fallback;
}

inline uint8_t poweroffShadeMask(uint8_t shade, int row, int col) {
    const bool odd = ((row + col) & 1) != 0;
    switch (shade) {
        case 1: return odd ? 0b101011 : 0b110101;
        case 2: return odd ? 0b111011 : 0b110111;
        default: return 0b111111;
    }
}

inline bool poweroffBitOn(uint8_t bits, int gx, int gy) {
    int bit = (POWEROFF_ART_PX_H - 1 - gy) * POWEROFF_ART_PX_W + (POWEROFF_ART_PX_W - 1 - gx);
    return (bits & (1 << bit)) != 0;
}

inline void poweroffDriveAndHold(int pin, bool level_high) {
    if (pin < 0) return;
    pinMode(pin, OUTPUT);
    digitalWrite(pin, level_high ? HIGH : LOW);
    gpio_hold_en((gpio_num_t)pin);
}

inline void poweroffQuiesceHardware() {
    // Turn off controllable module rails and indicator load switches.
    poweroffDriveAndHold(BOARD_LORA_EN, LOW);
    poweroffDriveAndHold(BOARD_GPS_EN, LOW);
    poweroffDriveAndHold(BOARD_1V8_EN, LOW);
    poweroffDriveAndHold(BOARD_KEYBOARD_LED, LOW);

    // Keep shared SPI devices deselected and radio reset asserted.
    poweroffDriveAndHold(BOARD_LORA_CS, HIGH);
    poweroffDriveAndHold(BOARD_SD_CS, HIGH);
    poweroffDriveAndHold(BOARD_EPD_CS, HIGH);
    poweroffDriveAndHold(BOARD_LORA_RST, LOW);

    // Persist these GPIO levels through deep sleep.
    gpio_deep_sleep_hold_en();
}

void powerOff() {
    // Render ASCII art as pixel bitmap on e-ink, then deep sleep
    const int art_height = POWEROFF_ART_LINES * POWEROFF_ART_PX_H;
    const int y_offset = (SCREEN_H - art_height) / 2;

    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        const char* p = poweroff_art;
        int row = 0;
        int col = 0;
        while (*p) {
            if (*p == '\n') {
                row++;
                col = 0;
            } else {
                const PoweroffBlockGlyph& glyph = poweroffBlockGlyphFor(*p);
                if (glyph.shape_bits != 0) {
                    int cell_x = col * POWEROFF_ART_PX_W;
                    int cell_y = y_offset + row * POWEROFF_ART_PX_H;
                    uint8_t draw_bits = glyph.shape_bits & poweroffShadeMask(glyph.shade, row, col);

                    for (int gy = 0; gy < POWEROFF_ART_PX_H; gy++) {
                        for (int gx = 0; gx < POWEROFF_ART_PX_W; gx++) {
                            if (!poweroffBitOn(draw_bits, gx, gy)) continue;
                            int px = cell_x + gx;
                            int py = cell_y + gy;
                            if (px < 0 || py < 0 || px >= SCREEN_W || py >= SCREEN_H) continue;
                            display.drawPixel(px, py, GxEPD_BLACK);
                        }
                    }
                }
                col++;
            }
            p++;
        }
    } while (display.nextPage());

    delay(100);
    display.hibernate();

    // Stop BLE advertising/connection before sleeping.
    btShutdown();

    // Stop shared buses before sleeping.
    if (sd_mounted) SD.end();
    SPI.end();
    Wire.end();

    // Disconnect WiFi
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    // Drive externally switched rails fully off and keep them latched in sleep.
    poweroffQuiesceHardware();

    // Enter deep sleep with no wakeup source (only reset wakes)
    esp_deep_sleep_start();
}

// --- Command Processor ---

void wifiScanCommand() {
    WiFi.mode(WIFI_STA);
    int n = WiFi.scanNetworks();
    if (n < 0) {
        cmdSetResult("Scan failed");
        return;
    }
    if (n == 0) {
        cmdSetResult("No WiFi networks found");
        return;
    }

    cmdClearResult();
    cmdAddLine("Scan: %d network(s)", n);
    for (int i = 0; i < n && i < 6; i++) {
        int cfg_idx = wifiConfigIndexForSSID(WiFi.SSID(i));
        cmdAddLine("%c %s %ddBm",
                   cfg_idx >= 0 ? '*' : ' ',
                   WiFi.SSID(i).c_str(),
                   WiFi.RSSI(i));
    }

    bool connected = false;
    bool tried_known = false;
    bool tried_cfg[MAX_WIFI_APS] = { false };
    for (;;) {
        int best_cfg = -1;
        int best_rssi = -1000;
        for (int i = 0; i < n; i++) {
            int cfg_idx = wifiConfigIndexForSSID(WiFi.SSID(i));
            if (cfg_idx < 0 || tried_cfg[cfg_idx]) continue;
            if (WiFi.RSSI(i) > best_rssi) {
                best_rssi = WiFi.RSSI(i);
                best_cfg = cfg_idx;
            }
        }
        if (best_cfg < 0) break;

        tried_known = true;
        tried_cfg[best_cfg] = true;
        cmdAddLine("Try: %s", config_wifi[best_cfg].ssid);
        WiFiAttemptResult attempt = wifiTryAP(config_wifi[best_cfg].ssid, config_wifi[best_cfg].pass, WIFI_CONNECT_TIMEOUT_MS);
        if (attempt.connected) {
            connected = true;
            wifi_state = WIFI_CONNECTED;
            wifiClearLastFailure();
            cmdAddLine("WiFi: %s", WiFi.localIP().toString().c_str());
            break;
        } else {
            char reason[48];
            wifiFormatFailureReason(attempt, reason, sizeof(reason));
            wifi_state = WIFI_FAILED;
            wifiSetLastFailure(config_wifi[best_cfg].ssid, reason);
            cmdAddLine("  fail: %s", reason);
        }
    }

    if (!connected) {
        if (WiFi.status() == WL_CONNECTED) {
            wifi_state = WIFI_CONNECTED;
            cmdAddLine("WiFi: %s", WiFi.SSID().c_str());
        } else {
            wifi_state = WIFI_FAILED;
            if (!tried_known) {
                cmdAddLine("No known SSIDs in scan");
            } else {
                cmdAddLine("Known APs failed");
                if (wifi_last_fail_ssid[0] != '\0') {
                    cmdAddLine("%s: %s", wifi_last_fail_ssid, wifi_last_fail_reason);
                }
            }
        }
    }

    WiFi.scanDelete();
}

void executeCommand(const char* cmd) {
    // Parse command word and argument
    char word[CMD_BUF_LEN + 1];
    char arg[CMD_BUF_LEN + 1];
    word[0] = '\0';
    arg[0] = '\0';

    while (*cmd == ' ') cmd++;
    if (*cmd == '\0') { cmd_result_valid = false; return; }

    int wi = 0;
    while (*cmd && *cmd != ' ' && wi < CMD_BUF_LEN) word[wi++] = *cmd++;
    word[wi] = '\0';
    while (*cmd == ' ') cmd++;
    strncpy(arg, cmd, CMD_BUF_LEN);
    arg[CMD_BUF_LEN] = '\0';

    // --- File commands (flat, all files at /) ---
    if (strcmp(word, "l") == 0 || strcmp(word, "ls") == 0) {
        int n = listDirectory("/");
        if (n < 0) {
            cmdSetResult("Can't read SD");
        } else if (n == 0) {
            cmdSetResult("(empty)");
        } else {
            cmdClearResult();
            for (int i = 0; i < n && i < CMD_RESULT_LINES; i++) {
                if (file_list[i].is_dir) {
                    cmdAddLine("[%s]", file_list[i].name);
                } else {
                    cmdAddLine("%s %dB", file_list[i].name, (int)file_list[i].size);
                }
            }
            if (n > CMD_RESULT_LINES) cmdAddLine("... +%d more", n - CMD_RESULT_LINES);
        }
    } else if (strcmp(word, "e") == 0 || strcmp(word, "edit") == 0) {
        if (arg[0] == '\0') {
            cmdEditPickerStart();
        } else {
            cmdEditPickerStop();
            autoSaveDirty();
            String path = "/" + String(arg);
            if (loadFromFile(path.c_str())) {
                current_file = path;
                cmdSetResult("Loaded %s (%d B)", arg, text_len);
                app_mode = MODE_NOTEPAD;
            } else {
                text_len = 0; cursor_pos = 0; scroll_line = 0;
                text_buf[0] = '\0';
                current_file = path;
                file_modified = false;
                cmdSetResult("New: %s", arg);
                app_mode = MODE_NOTEPAD;
            }
        }
    } else if (strcmp(word, "w") == 0 || strcmp(word, "save") == 0) {
        if (arg[0] != '\0') {
            current_file = "/" + String(arg);
        } else if (current_file.length() == 0) {
            current_file = "/UNSAVED";
        }
        if (saveToFile(current_file.c_str())) {
            cmdSetResult("Saved %s (%d B)", current_file.c_str(), text_len);
        } else {
            cmdSetResult("Save failed");
        }
    } else if (strcmp(word, "n") == 0 || strcmp(word, "new") == 0) {
        autoSaveDirty();
        text_len = 0; cursor_pos = 0; scroll_line = 0;
        text_buf[0] = '\0';
        current_file = "";
        file_modified = false;
        cmdSetResult("New buffer");
        app_mode = MODE_NOTEPAD;
    } else if (strcmp(word, "r") == 0 || strcmp(word, "rm") == 0) {
        if (arg[0] == '\0') { cmdSetResult("r <name>"); }
        else {
            String path = "/" + String(arg);
            sdAcquire();
            bool ok = SD.remove(path.c_str());
            sdRelease();
            cmdSetResult(ok ? "Removed %s" : "Failed: %s", arg);
        }
    } else if (strcmp(word, "u") == 0 || strcmp(word, "upload") == 0) {
        if (!ssh_connected) {
            cmdSetResult("SSH not connected");
        } else if (download_running) {
            cmdSetResult("Transfer in progress...");
        } else if (upload_running) {
            cmdSetResult("Upload in progress...");
        } else {
            xTaskCreatePinnedToCore(uploadTask, "upload", 16384, NULL, 1, NULL, 1);
            cmdSetResult("Starting upload...");
        }
    } else if (strcmp(word, "d") == 0 || strcmp(word, "download") == 0) {
        if (!ssh_connected) {
            cmdSetResult("SSH not connected");
        } else if (upload_running) {
            cmdSetResult("Transfer in progress...");
        } else if (download_running) {
            cmdSetResult("Download in progress...");
        } else {
            xTaskCreatePinnedToCore(downloadTask, "download", 16384, NULL, 1, NULL, 1);
            cmdSetResult("Starting download...");
        }
    } else if (strcmp(word, "k") == 0 || strcmp(word, "keyboard") == 0) {
        if (cmd_return_mode == MODE_KEYBOARD) {
            AppMode back = keyboard_return_mode;
            if (back == MODE_COMMAND || back == MODE_KEYBOARD) back = MODE_NOTEPAD;
            cmd_return_mode = back;
            app_mode = back;
            cmdSetResult("Keyboard mode off");
        } else {
            keyboard_return_mode = cmd_return_mode;
            cmd_return_mode = MODE_KEYBOARD;
            app_mode = MODE_KEYBOARD;
            if (btIsConnected()) cmdSetResult("Keyboard mode on");
            else cmdSetResult("Keyboard mode on (BT waiting)");
        }
    }
    // --- Other commands ---
    else if (strcmp(word, "p") == 0 || strcmp(word, "paste") == 0) {
        if (text_len == 0) {
            cmdSetResult("Notepad empty");
        } else if (cmd_return_mode == MODE_KEYBOARD) {
            size_t sent = btTypeTextN(text_buf, (size_t)text_len, 0);
            if (sent == 0) cmdSetResult("BT not connected");
            else cmdSetResult("BT pasted %d/%d chars", (int)sent, text_len);
        } else if (!ssh_connected || !ssh_chan) {
            cmdSetResult("SSH not connected");
        } else {
            for (int i = 0; i < text_len; i += 64) {
                int chunk = (text_len - i > 64) ? 64 : (text_len - i);
                ssh_channel_write(ssh_chan, &text_buf[i], chunk);
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            cmdSetResult("Pasted %d chars", text_len);
        }
    } else if (strcmp(word, "dc") == 0) {
        sshDisconnect();
        cmdSetResult("Disconnected");
    } else if (strcmp(word, "ws") == 0 || strcmp(word, "scan") == 0) {
        wifiScanCommand();
    } else if (strcmp(word, "f") == 0 || strcmp(word, "refresh") == 0) {
        partial_count = 100;
        cmdSetResult("Full refresh queued");
    } else if (strcmp(word, "bt") == 0 || strcmp(word, "bluetooth") == 0) {
        if (arg[0] == '\0' || strcmp(arg, "status") == 0) {
            cmdClearResult();
            cmdAddLine("BT:%s %s", btStatusShort(), btIsBonded() ? "bonded" : "unpaired");
            cmdAddLine("Name:%s", config_bt_name);
            cmdAddLine("Mode:HID keyboard");
            if (btPeerAddress()[0] != '\0') {
                cmdAddLine("Peer:%s", btPeerAddress());
            }
            if (config_bt_passkey >= 100000 && config_bt_passkey <= 999999) {
                cmdAddLine("PIN:%06u", (unsigned)config_bt_passkey);
            }
        } else if (strncmp(arg, "send ", 5) == 0) {
            size_t sent = btTypeText(arg + 5);
            if (sent > 0) cmdSetResult("BT sent %d", (int)sent);
            else cmdSetResult("BT not connected");
        } else {
            cmdSetResult("bt status|send <txt>");
        }
    } else if (strcmp(word, "s") == 0 || strcmp(word, "status") == 0) {
        const char* ws = "off";
        if (wifi_state == WIFI_CONNECTED) ws = "ok";
        else if (wifi_state == WIFI_CONNECTING) ws = "...";
        else if (wifi_state == WIFI_FAILED) ws = "fail";
        cmdClearResult();
        cmdAddLine("WiFi:%s SSH:%s BT:%s", ws, ssh_connected ? "ok" : "off", btStatusShort());
        if (wifi_last_fail_ssid[0] != '\0') {
            cmdAddLine("WiFi fail:%s", wifi_last_fail_ssid);
            cmdAddLine("Why:%s", wifi_last_fail_reason);
        }
        if (btPeerAddress()[0] != '\0') {
            cmdAddLine("BT peer:%s", btPeerAddress());
        }
        cmdAddLine("Bat:%d%% Heap:%dK", battery_pct, ESP.getFreeHeap() / 1024);
        if (current_file.length() > 0) cmdAddLine("File:%s%s", current_file.c_str(), file_modified ? "*" : "");
    } else if (strcmp(word, "off") == 0) {
        poweroff_requested = true;
    } else if (strcmp(word, "?") == 0 || strcmp(word, "h") == 0 || strcmp(word, "help") == 0) {
        cmdClearResult();
        cmdAddLine("(l)ist (e)dit (w)rite (n)ew");
        cmdAddLine("(r)m (u)pload (d)ownload");
        cmdAddLine("(k)eyboard (p)aste dc (ws)/scan bt");
        cmdAddLine("re(f)resh (s)tatus off (h)elp");
    } else {
        cmdSetResult("Unknown: %s (?=help)", word);
    }
}

bool handleCommandKeyPress(int event_code) {
    int key_num = (event_code & 0x7F);
    int idx = key_num - 1;
    int row = idx / KEYPAD_COLS;
    int col_raw = idx % KEYPAD_COLS;
    int col_rev = (KEYPAD_COLS - 1) - col_raw;

    if (row < 0 || row >= KEYPAD_ROWS || col_rev < 0 || col_rev >= KEYPAD_COLS) return false;

    if (cmd_edit_picker_active) {
        if (IS_MIC(row, col_rev)) {
            cmdEditPickerStop();
            app_mode = cmd_return_mode;
            return false;
        }
        if (IS_SHIFT(row, col_rev) || IS_SYM(row, col_rev) || IS_ALT(row, col_rev) || IS_DEAD(row, col_rev)) {
            return false;
        }

        char base = keymap_lower[row][col_rev];
        if (base == 0) return false;
        if (base == 'w') return cmdEditPickerMoveSelection(-1);
        if (base == 's') return cmdEditPickerMoveSelection(1);
        if (base == 'a') return cmdEditPickerPage(-1);
        if (base == 'd') return cmdEditPickerPage(1);
        if (base == '\n') return cmdEditPickerOpenSelected();
        if (base == '\b') {
            cmdEditPickerStop();
            cmdSetResult("Edit cancelled");
            return true;
        }
        return false;
    }

    if (IS_SHIFT(row, col_rev)) { shift_held = !shift_held; return false; }
    if (IS_SYM(row, col_rev))   { sym_mode = true; return true; }
    if (IS_ALT(row, col_rev))   { return false; }
    if (IS_MIC(row, col_rev))   {
        if (sym_mode) {
            sym_mode = false;
            if (cmd_len < CMD_BUF_LEN) {
                cmd_buf[cmd_len++] = '0';
                cmd_buf[cmd_len] = '\0';
                return true;
            }
            return false;
        }
        app_mode = cmd_return_mode;
        return false;
    }
    if (IS_DEAD(row, col_rev))  { return false; }

    char c;
    if (sym_mode)        { c = keymap_sym[row][col_rev]; sym_mode = false; }
    else if (shift_held) c = keymap_upper[row][col_rev];
    else                 c = keymap_lower[row][col_rev];

    if (c == 0) return false;

    if (c == '\b') {
        if (cmd_len > 0) {
            cmd_len--;
            cmd_buf[cmd_len] = '\0';
            return true;
        }
        return false;
    }

    if (c == '\n') {
        executeCommand(cmd_buf);
        cmd_len = 0;
        cmd_buf[0] = '\0';
        return true;
    }

    if (c >= ' ' && c <= '~' && cmd_len < CMD_BUF_LEN) {
        cmd_buf[cmd_len++] = c;
        cmd_buf[cmd_len] = '\0';
        if (shift_held) shift_held = false;
        return true;
    }

    return false;
}

void renderCommandPrompt() {
    partial_count++;
    // Half screen: top half stays (notepad/terminal content), bottom half is command area
    int cmd_area_y = SCREEN_H / 2;
    int region_h = SCREEN_H - cmd_area_y;

    display.setPartialWindow(0, cmd_area_y, SCREEN_W, region_h);
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.setTextColor(GxEPD_BLACK);
        display.setFont(NULL);

        // Separator line
        display.drawLine(0, cmd_area_y, SCREEN_W, cmd_area_y, GxEPD_BLACK);

        int y = cmd_area_y + 2;

        // Show multi-line result
        if (cmd_result_valid) {
            for (int i = 0; i < cmd_result_count; i++) {
                display.setCursor(MARGIN_X, y);
                display.print(cmd_result[i]);
                y += CHAR_H;
                if (y >= SCREEN_H - STATUS_H - CHAR_H - 2) break;
            }
        }

        // Draw prompt line at bottom of command area (above status bar)
        int py = SCREEN_H - STATUS_H - CHAR_H - 2;
        display.setCursor(MARGIN_X, py);
        if (cmd_edit_picker_active) {
            display.print("> edit");
        } else {
            display.print("> ");
            display.print(cmd_buf);
            // Cursor
            int cx = MARGIN_X + (cmd_len + 2) * CHAR_W;
            display.fillRect(cx, py - 1, CHAR_W, CHAR_H, GxEPD_BLACK);
        }

        // Status bar
        int bar_y = SCREEN_H - STATUS_H;
        display.fillRect(0, bar_y, SCREEN_W, STATUS_H, GxEPD_BLACK);
        display.setTextColor(GxEPD_WHITE);
        display.setCursor(2, bar_y + 1);
        if (upload_running) {
            uint32_t done = upload_bytes_done;
            uint32_t total = upload_bytes_total;
            uint32_t elapsed_ms = millis() - upload_started_ms;
            uint32_t rate = (elapsed_ms > 0)
                ? (uint32_t)(((uint64_t)done * 1000ULL) / elapsed_ms)
                : 0;
            char done_s[12], total_s[12], rate_s[12];
            char ul[56];
            formatBytesCompact(done, done_s, sizeof(done_s));
            formatBytesCompact(total, total_s, sizeof(total_s));
            formatBytesCompact(rate, rate_s, sizeof(rate_s));
            snprintf(ul, sizeof(ul), "U %d/%d %s/%s %s/s",
                     (int)upload_done_count, (int)upload_total_count,
                     done_s, total_s, rate_s);
            display.print(ul);
        } else if (download_running) {
            uint32_t done = download_bytes_done;
            uint32_t total = download_bytes_total;
            uint32_t elapsed_ms = millis() - download_started_ms;
            uint32_t rate = (elapsed_ms > 0)
                ? (uint32_t)(((uint64_t)done * 1000ULL) / elapsed_ms)
                : 0;
            char done_s[12], total_s[12], rate_s[12];
            char dl[56];
            formatBytesCompact(done, done_s, sizeof(done_s));
            formatBytesCompact(total, total_s, sizeof(total_s));
            formatBytesCompact(rate, rate_s, sizeof(rate_s));
            snprintf(dl, sizeof(dl), "D %d/%d %s/%s %s/s",
                     (int)download_done_count, (int)download_total_count,
                     done_s, total_s, rate_s);
            display.print(dl);
        } else if (cmd_edit_picker_active) {
            display.print("[PICK] WASD nav ENTER open");
        } else {
            if (cmd_return_mode == MODE_KEYBOARD) display.print("[CMD] k exit | p paste | MIC return");
            else display.print("[CMD] ? help | MIC exit");
        }
    } while (display.nextPage());
}
