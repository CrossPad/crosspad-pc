/**
 * @file    twin_account.cpp
 * @brief   The board's crosspad.app account and remote management, for the
 *          twin: a board that was never paired. Its Settings pages (WiFi,
 *          Account) are built here unchanged; the browser has no radio and
 *          the twin link carries no token, so the board reads unlinked, a
 *          pairing is refused and remote management never wants the network.
 */
#include <cstring>

#include "cp_account.h"
#include "cp_remote.h"
#include "acct_pair_overlay.h"

bool cp_account_linked(void) { return false; }
void cp_account_name(char *out, size_t n) { if (n) out[0] = '\0'; }
bool cp_account_notice(void) { return false; }
bool cp_account_pair_start(char *refusal, size_t n)
{
    if (n) { std::strncpy(refusal, "not in the browser", n - 1); refusal[n - 1] = '\0'; }
    return false;
}
void cp_account_pair_cancel(void) {}
bool cp_account_unlink(void) { return false; }

bool        cp_remote_wanted(void) { return false; }
void        cp_remote_retry_now(void) {}
const char *cp_remote_state_name(void) { return "off"; }

void acct_pair_overlay_open(void) {}
void acct_pair_overlay_close(void) {}
