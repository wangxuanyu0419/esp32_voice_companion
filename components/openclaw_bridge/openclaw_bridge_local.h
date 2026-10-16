#ifndef OPENCLAW_BRIDGE_LOCAL_H
#define OPENCLAW_BRIDGE_LOCAL_H

#define CLAWCHAT_WS_HOST "clawchat.xuanyu.uk"
#define CLAWCHAT_WS_PORT 443
#define CLAWCHAT_WS_SECURE 1
#if __has_include("openclaw_secrets.local.h")
#include "openclaw_secrets.local.h"
#endif
#ifndef CLAWCHAT_APP_TOKEN
#define CLAWCHAT_APP_TOKEN ""
#endif

#endif
