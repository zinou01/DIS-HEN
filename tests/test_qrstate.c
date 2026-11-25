#include "../installer/qrstate.h"
#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(c, n) do { if(!(c)){ printf("FAIL %s\n", n); g_fail++; } } while(0)

static int P(const char *j, qrstate_t *s){ return qrstate_parse(j, strlen(j), s); }

int main(void){
    qrstate_t s;
    CHECK(P("{\"v\":1,\"tick\":7,\"state\":\"idle\",\"url\":\"\",\"user\":\"\",\"err\":\"\"}", &s) == 0 && s.stage == QS_IDLE && s.tick == 7, "idle");
    CHECK(!qrstate_busy(&s), "idle not busy");
    CHECK(P("{\"tick\":8,\"state\":\"qr\",\"url\":\"https://discord.com/ra/AbC-1_x\"}", &s) == 0 && s.stage == QS_QR && qrstate_busy(&s), "qr");
    CHECK(strcmp(s.url, "https://discord.com/ra/AbC-1_x") == 0, "qr url");
    CHECK(P("{\"tick\":8,\"state\":\"qr\",\"url\":\"https://evil.example/ra/x\"}", &s) != 0 && s.stage == QS_UNKNOWN, "foreign url refused");
    CHECK(P("{\"tick\":8,\"state\":\"qr\",\"url\":\"\"}", &s) != 0, "empty qr url refused");
    CHECK(P("{\"tick\":9,\"state\":\"scanned\",\"user\":\"Test User\"}", &s) == 0 && s.stage == QS_SCANNED && strcmp(s.user, "Test User") == 0, "scanned");
    CHECK(P("{\"tick\":10,\"state\":\"done\"}", &s) == 0 && s.stage == QS_DONE && !qrstate_busy(&s), "done");
    CHECK(P("{\"tick\":11,\"state\":\"error\",\"err\":\"captcha\"}", &s) == 0 && s.stage == QS_ERROR && strstr(qrstate_detail(&s), "captcha"), "captcha text");
    CHECK(P("{\"tick\":11,\"state\":\"error\",\"err\":\"net\"}", &s) == 0 && strstr(qrstate_detail(&s), "internet"), "net text");
    CHECK(P("{\"tick\":1,\"state\":\"future\"}", &s) == 0 && s.stage == QS_UNKNOWN, "unknown state kept unknown");
    CHECK(P("not json", &s) != 0, "garbage");
    CHECK(P("[]", &s) != 0, "array");
    CHECK(P("{\"state\":\"idle\"}", &s) != 0, "missing tick");
    CHECK(P("{\"tick\":-1,\"state\":\"idle\"}", &s) != 0, "negative tick");
    CHECK(P("{\"tick\":1,\"state\":5}", &s) != 0, "non-string state");
    CHECK(qrstate_parse(NULL, 0, &s) != 0, "null");
    CHECK(qrstate_parse("{}", 2, NULL) != 0, "null out");
    CHECK(qrstate_load("/nonexistent/qr_state.json", &s) != 0, "missing file");
    /* oversized fields are truncated, not overflowed */
    char big[600];
    char longurl[400]; memset(longurl, 'a', sizeof longurl - 1); longurl[sizeof longurl - 1] = 0;
    snprintf(big, sizeof big, "{\"tick\":1,\"state\":\"scanned\",\"user\":\"%s\"}", longurl);
    CHECK(P(big, &s) == 0 && strlen(s.user) == sizeof s.user - 1, "long user truncated");
    CHECK(qrstate_headline(NULL) != NULL && qrstate_detail(NULL) != NULL, "null-safe text");
    if(g_fail){ printf("qrstate tests FAILED (%d)\n", g_fail); return 1; }
    printf("qrstate tests passed\n");
    return 0;
}
