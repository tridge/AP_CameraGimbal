#pragma once
#include "APC_StringBuffer.h"

// Minimal Z1 firmware recovery UI. It must not read templates, CSS or JS from
// the installed webroot. Authentication and installation remain server-owned.
class APC_RecoveryPage {
public:
    static char *render(bool login, const char *token, const char *message, size_t *length)
    {
        APC_StringBuffer page;
        page.append("<!doctype html><html lang=en><meta charset=utf-8>"
                    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
                    "<title>Z1-Mini firmware recovery</title><body>"
                    "<h1>Z1-Mini firmware recovery</h1>"
                    "<p>The web interface files are missing or unavailable. "
                    "Upload a complete Z1Mini_AP_*.gcu firmware package to restore them.</p>");
        if (message) {
            page.append("<p role=alert>");
            page.append_html(message);
            page.append("</p>");
        }
        if (login) {
            page.appendf("<form method=post action=/login>"
                         "<input type=hidden name=token value=\"%s\">"
                         "<p><label>Username <input name=username value=admin autocomplete=username required></label></p>"
                         "<p><label>Password <input name=password type=password autocomplete=current-password required></label></p>"
                         "<button type=submit>Log in</button></form>", token);
        } else {
            page.append("<form id=firmware-upload method=post action=/upgrade>"
                        "<input id=firmware type=file accept=.gcu hidden>"
                        "<button id=select-firmware type=button>Upload firmware</button>"
                        "<progress id=firmware-progress value=0 max=100 hidden></progress>"
                        "<p><output id=firmware-status role=status></output></p></form>");
            page.appendf("<script src=/upgrade.js data-csrf=\"%s\" defer></script>", token);
            page.append("<noscript>Enable JavaScript to upload firmware.</noscript>"
                        "<p>Settings and credentials are preserved. The camera restarts after installation.</p>"
                        "<p><a href=/>Open main page</a></p>");
        }
        page.append("</body></html>");
        *length = page.size();
        return page.release();
    }
};
