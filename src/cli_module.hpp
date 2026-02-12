// --- Command Processor ---

// --- SCP Upload ---

static volatile bool upload_running = false;
static volatile int upload_done_count = 0;
static volatile int upload_total_count = 0;

bool scpPushFile(ssh_scp scp, const char* sd_path, const char* remote_name) {
    sdAcquire();
    File f = SD.open(sd_path, FILE_READ);
    if (!f) { sdRelease(); return false; }
    size_t sz = f.size();

    // Read entire file into memory (files are small, max 4K)
    char buf[MAX_TEXT_LEN];
    int n = f.read((uint8_t*)buf, sz);
    f.close();
    sdRelease();

    if (n <= 0) return false;
    if (ssh_scp_push_file(scp, remote_name, n, 0644) != SSH_OK) return false;
    return ssh_scp_write(scp, buf, n) == SSH_OK;
}

void uploadTask(void* param) {
    upload_running = true;
    upload_done_count = 0;

    // List flat files at root
    int n = listDirectory("/");
    upload_total_count = 0;
    for (int i = 0; i < n; i++) {
        if (!file_list[i].is_dir) upload_total_count++;
    }
    cmdSetResult("Uploading %d files...", upload_total_count);
    render_requested = true;

    // Create SCP session using existing SSH connection's session
    ssh_scp scp = ssh_scp_new(ssh_sess, SSH_SCP_WRITE, "tdeck");
    if (!scp) {
        cmdSetResult("SCP: init failed");
        render_requested = true;
        upload_running = false;
        vTaskDelete(NULL);
        return;
    }
    if (ssh_scp_init(scp) != SSH_OK) {
        cmdSetResult("SCP: %s", ssh_get_error(ssh_sess));
        ssh_scp_free(scp);
        render_requested = true;
        upload_running = false;
        vTaskDelete(NULL);
        return;
    }

    // Push each file
    for (int i = 0; i < n; i++) {
        if (file_list[i].is_dir) continue;
        String path = "/" + String(file_list[i].name);
        if (scpPushFile(scp, path.c_str(), file_list[i].name)) {
            upload_done_count++;
            render_requested = true;
        }
    }

    ssh_scp_close(scp);
    ssh_scp_free(scp);

    cmdSetResult("Upload done: %d files", upload_done_count);
    render_requested = true;
    upload_running = false;
    vTaskDelete(NULL);
}

// --- SCP Download ---

static volatile bool download_running = false;
static volatile int download_done_count = 0;
static volatile int download_total_count = 0;

bool scpPullFile(ssh_scp scp, const char* remote_name, size_t size) {
    char buf[MAX_TEXT_LEN];
    size_t remaining = size;
    size_t offset = 0;
    while (remaining > 0) {
        size_t chunk = remaining > sizeof(buf) ? sizeof(buf) : remaining;
        int n = ssh_scp_read(scp, buf + offset, chunk);
        if (n < 0) return false;
        offset += n;
        remaining -= n;
    }

    String path = "/" + String(remote_name);
    sdAcquire();
    File f = SD.open(path.c_str(), FILE_WRITE);
    if (!f) { sdRelease(); return false; }
    f.write((uint8_t*)buf, offset);
    f.close();
    sdRelease();
    return true;
}

void downloadTask(void* param) {
    download_running = true;
    download_done_count = 0;
    download_total_count = 0;

    ssh_scp scp = ssh_scp_new(ssh_sess, SSH_SCP_READ | SSH_SCP_RECURSIVE, "tdeck");
    if (!scp) {
        cmdSetResult("SCP: init failed");
        render_requested = true;
        download_running = false;
        vTaskDelete(NULL);
        return;
    }
    if (ssh_scp_init(scp) != SSH_OK) {
        cmdSetResult("SCP: %s", ssh_get_error(ssh_sess));
        ssh_scp_free(scp);
        render_requested = true;
        download_running = false;
        vTaskDelete(NULL);
        return;
    }

    cmdSetResult("Downloading...");
    render_requested = true;

    int r;
    while ((r = ssh_scp_pull_request(scp)) != SSH_SCP_REQUEST_EOF) {
        if (r == SSH_SCP_REQUEST_NEWDIR) {
            ssh_scp_accept_request(scp);
            continue;
        }
        if (r == SSH_SCP_REQUEST_ENDDIR) {
            continue;
        }
        if (r == SSH_SCP_REQUEST_NEWFILE) {
            size_t size = ssh_scp_request_get_size(scp);
            const char* name = ssh_scp_request_get_filename(scp);
            download_total_count++;
            ssh_scp_accept_request(scp);
            if (scpPullFile(scp, name, size)) {
                download_done_count++;
                render_requested = true;
            }
            continue;
        }
        if (r == SSH_ERROR) break;
    }

    ssh_scp_close(scp);
    ssh_scp_free(scp);

    cmdSetResult("Download done: %d files", download_done_count);
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
                if (*p != ' ') {
                    int px = col * POWEROFF_ART_PX_W;
                    int py = y_offset + row * POWEROFF_ART_PX_H;
                    if (px + POWEROFF_ART_PX_W <= SCREEN_W && py + POWEROFF_ART_PX_H <= SCREEN_H && py >= 0) {
                        display.fillRect(px, py, POWEROFF_ART_PX_W, POWEROFF_ART_PX_H, GxEPD_BLACK);
                    }
                }
                col++;
            }
            p++;
        }
    } while (display.nextPage());

    delay(100);
    display.hibernate();

    // Disconnect WiFi
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

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
        } else if (upload_running) {
            cmdSetResult("Upload in progress...");
        } else {
            char mkdir_cmd[64];
            snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p ~/tdeck\n");
            ssh_channel_write(ssh_chan, mkdir_cmd, strlen(mkdir_cmd));
            vTaskDelay(pdMS_TO_TICKS(500));
            char drain[256];
            ssh_channel_read_nonblocking(ssh_chan, drain, sizeof(drain), 0);
            xTaskCreatePinnedToCore(uploadTask, "upload", 16384, NULL, 1, NULL, 1);
            cmdSetResult("Starting upload...");
        }
    } else if (strcmp(word, "d") == 0 || strcmp(word, "download") == 0) {
        if (!ssh_connected) {
            cmdSetResult("SSH not connected");
        } else if (download_running) {
            cmdSetResult("Download in progress...");
        } else {
            xTaskCreatePinnedToCore(downloadTask, "download", 16384, NULL, 1, NULL, 1);
            cmdSetResult("Starting download...");
        }
    }
    // --- Other commands ---
    else if (strcmp(word, "p") == 0 || strcmp(word, "paste") == 0) {
        if (!ssh_connected || !ssh_chan) {
            cmdSetResult("SSH not connected");
        } else if (text_len == 0) {
            cmdSetResult("Notepad empty");
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
    } else if (strcmp(word, "s") == 0 || strcmp(word, "status") == 0) {
        const char* ws = "off";
        if (wifi_state == WIFI_CONNECTED) ws = "ok";
        else if (wifi_state == WIFI_CONNECTING) ws = "...";
        else if (wifi_state == WIFI_FAILED) ws = "fail";
        cmdClearResult();
        cmdAddLine("WiFi:%s SSH:%s", ws, ssh_connected ? "ok" : "off");
        if (wifi_last_fail_ssid[0] != '\0') {
            cmdAddLine("WiFi fail:%s", wifi_last_fail_ssid);
            cmdAddLine("Why:%s", wifi_last_fail_reason);
        }
        cmdAddLine("Bat:%d%% Heap:%dK", battery_pct, ESP.getFreeHeap() / 1024);
        if (current_file.length() > 0) cmdAddLine("File:%s%s", current_file.c_str(), file_modified ? "*" : "");
    } else if (strcmp(word, "off") == 0) {
        poweroff_requested = true;
    } else if (strcmp(word, "?") == 0 || strcmp(word, "h") == 0 || strcmp(word, "help") == 0) {
        cmdClearResult();
        cmdAddLine("(l)ist (e)dit (w)rite (n)ew");
        cmdAddLine("(r)m (u)pload (d)ownload");
        cmdAddLine("(p)aste dc (ws)/scan re(f)resh");
        cmdAddLine("(s)tatus off (h)elp");
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
            char ul[40];
            snprintf(ul, sizeof(ul), "Upload: %d/%d", (int)upload_done_count, (int)upload_total_count);
            display.print(ul);
        } else if (download_running) {
            char dl[40];
            snprintf(dl, sizeof(dl), "Download: %d/%d", (int)download_done_count, (int)download_total_count);
            display.print(dl);
        } else if (cmd_edit_picker_active) {
            display.print("[PICK] WASD nav ENTER open");
        } else {
            display.print("[CMD] ? help | MIC exit");
        }
    } while (display.nextPage());
}

