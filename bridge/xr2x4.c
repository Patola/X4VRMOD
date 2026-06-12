/*
 * xr2x4 — OpenXR headless HMD-pose → OpenTrack-UDP bridge for X4: Foundations
 *
 * Polls the headset pose from an OpenXR runtime (WiVRn / Monado) using the
 * XR_MND_headless extension (no swapchain, no rendering) and streams it as
 * OpenTrack "UDP over network" packets — six little-endian doubles:
 *   { x_cm, y_cm, z_cm, yaw_deg, pitch_deg, roll_deg }
 * X4: Foundations (>= 7.50) listens for exactly this protocol on UDP :4242
 * when "OpenTrack Support" is enabled in Controls, so no OpenTrack install
 * is required in between.
 *
 * Build:   make            (gcc -O2 xr2x4.c -lopenxr_loader -lm)
 * Run:     XR_RUNTIME_JSON=/usr/share/openxr/1/openxr_wivrn.json ./xr2x4
 *
 * Recenter: press Enter in the terminal, or `kill -USR1 <pid>`.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define XR_USE_TIMESPEC 1
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

static volatile sig_atomic_t g_recenter = 1; /* capture origin on first pose */
static volatile sig_atomic_t g_quit = 0;

static void on_usr1(int sig) { (void)sig; g_recenter = 1; }
static void on_int(int sig)  { (void)sig; g_quit = 1; }

/* ---------- tiny quaternion helpers (x,y,z,w like XrQuaternionf) ---------- */

typedef struct { double x, y, z, w; } quatd;
typedef struct { double x, y, z; } vec3d;

static quatd q_conj(quatd q) { return (quatd){-q.x, -q.y, -q.z, q.w}; }

static quatd q_mul(quatd a, quatd b)
{
    return (quatd){
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

static vec3d q_rotate(quatd q, vec3d v)
{
    quatd p = {v.x, v.y, v.z, 0.0};
    quatd r = q_mul(q_mul(q, p), q_conj(q));
    return (vec3d){r.x, r.y, r.z};
}

/*
 * OpenXR space: +x right, +y up, -z forward.
 * Remap to aerospace (x fwd, y right, z down):  (x,y,z)a = (-z, x, -y)xr
 * — a proper rotation, so the quaternion vector part permutes the same way —
 * then use standard ZYX (yaw-pitch-roll) extraction.
 */
static void q_to_ypr_deg(quatd q, double *yaw, double *pitch, double *roll)
{
    double qw = q.w, qx = -q.z, qy = q.x, qz = -q.y;

    double sp = 2.0 * (qw * qy - qz * qx);
    if (sp > 1.0) sp = 1.0;
    if (sp < -1.0) sp = -1.0;

    *yaw   = atan2(2.0 * (qw * qz + qx * qy), 1.0 - 2.0 * (qy * qy + qz * qz)) * 180.0 / M_PI;
    *pitch = asin(sp) * 180.0 / M_PI;
    *roll  = atan2(2.0 * (qw * qx + qy * qz), 1.0 - 2.0 * (qx * qx + qy * qy)) * 180.0 / M_PI;
}

/* ------------------------------- options --------------------------------- */

static struct {
    const char *host;
    int port;
    double rate_hz;
    double pos_scale;          /* meters -> cm by default */
    int ix, iy, iz, iyaw, ipitch, iroll; /* per-axis sign flips */
    bool verbose;
} opt = {
    .host = "127.0.0.1",
    .port = 4242,
    .rate_hz = 120.0,
    .pos_scale = 100.0,
    .ix = 1, .iy = 1, .iz = 1, .iyaw = 1, .ipitch = 1, .iroll = 1,
    .verbose = false,
};

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [-H host] [-p port] [-r rate_hz] [-v]\n"
        "          [--ix] [--iy] [--iz] [--iyaw] [--ipitch] [--iroll]\n"
        "  -H host       UDP target (default 127.0.0.1)\n"
        "  -p port       UDP port (default 4242 = X4 OpenTrack listener)\n"
        "  -r rate_hz    send rate (default 120)\n"
        "  --i<axis>     flip the sign of one axis if in-game motion is mirrored\n"
        "  -v            print poses to stdout\n"
        "Recenter: Enter key or SIGUSR1.\n", argv0);
}

static bool parse_args(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-H") && i + 1 < argc)      opt.host = argv[++i];
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) opt.port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) opt.rate_hz = atof(argv[++i]);
        else if (!strcmp(argv[i], "--ix"))     opt.ix = -1;
        else if (!strcmp(argv[i], "--iy"))     opt.iy = -1;
        else if (!strcmp(argv[i], "--iz"))     opt.iz = -1;
        else if (!strcmp(argv[i], "--iyaw"))   opt.iyaw = -1;
        else if (!strcmp(argv[i], "--ipitch")) opt.ipitch = -1;
        else if (!strcmp(argv[i], "--iroll"))  opt.iroll = -1;
        else if (!strcmp(argv[i], "-v"))       opt.verbose = true;
        else { usage(argv[0]); return false; }
    }
    return true;
}

/* --------------------------------- main ---------------------------------- */

#define XR_CHECK(call, what)                                                  \
    do {                                                                      \
        XrResult _r = (call);                                                 \
        if (XR_FAILED(_r)) {                                                  \
            char _buf[XR_MAX_RESULT_STRING_SIZE] = "?";                       \
            if (instance != XR_NULL_HANDLE)                                   \
                xrResultToString(instance, _r, _buf);                         \
            fprintf(stderr, "FATAL: %s failed: %s (%d)\n", what, _buf, _r);   \
            goto cleanup;                                                     \
        }                                                                     \
    } while (0)

int main(int argc, char **argv)
{
    if (!parse_args(argc, argv)) return 2;

    signal(SIGUSR1, on_usr1);
    signal(SIGINT, on_int);
    signal(SIGTERM, on_int);

    /* non-blocking stdin so Enter recenters */
    fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);

    /* UDP socket */
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return 1; }
    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = htons((uint16_t)opt.port);
    if (inet_pton(AF_INET, opt.host, &dst.sin_addr) != 1) {
        fprintf(stderr, "bad host: %s\n", opt.host);
        return 1;
    }

    XrInstance instance = XR_NULL_HANDLE;
    XrSession session = XR_NULL_HANDLE;
    XrSpace view_space = XR_NULL_HANDLE, ref_space = XR_NULL_HANDLE;

    const char *exts[] = {
        XR_MND_HEADLESS_EXTENSION_NAME,
        XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME,
    };
    XrInstanceCreateInfo ici = {
        .type = XR_TYPE_INSTANCE_CREATE_INFO,
        .applicationInfo = {
            .applicationName = "xr2x4",
            .applicationVersion = 1,
            .engineName = "none",
            .apiVersion = XR_API_VERSION_1_0,
        },
        .enabledExtensionCount = 2,
        .enabledExtensionNames = exts,
    };
    XR_CHECK(xrCreateInstance(&ici, &instance),
             "xrCreateInstance (is WiVRn/Monado running? set XR_RUNTIME_JSON; "
             "needs XR_MND_headless + XR_KHR_convert_timespec_time)");

    PFN_xrConvertTimespecTimeToTimeKHR ts2time = NULL;
    XR_CHECK(xrGetInstanceProcAddr(instance, "xrConvertTimespecTimeToTimeKHR",
                                   (PFN_xrVoidFunction *)&ts2time),
             "xrGetInstanceProcAddr(xrConvertTimespecTimeToTimeKHR)");

    XrSystemGetInfo sgi = {
        .type = XR_TYPE_SYSTEM_GET_INFO,
        .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY,
    };
    XrSystemId system_id = XR_NULL_SYSTEM_ID;
    XR_CHECK(xrGetSystem(instance, &sgi, &system_id),
             "xrGetSystem (is the headset connected/streaming?)");

    XrSystemProperties sysprops = {.type = XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_SUCCEEDED(xrGetSystemProperties(instance, system_id, &sysprops)))
        fprintf(stderr, "HMD: %s\n", sysprops.systemName);

    /* headless: no graphics binding needed */
    XrSessionCreateInfo sci = {
        .type = XR_TYPE_SESSION_CREATE_INFO,
        .systemId = system_id,
    };
    XR_CHECK(xrCreateSession(instance, &sci, &session), "xrCreateSession");

    XrReferenceSpaceCreateInfo rsci = {
        .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
        .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL,
        .poseInReferenceSpace = {.orientation = {.w = 1.0f}},
    };
    XR_CHECK(xrCreateReferenceSpace(session, &rsci, &ref_space),
             "xrCreateReferenceSpace(LOCAL)");
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XR_CHECK(xrCreateReferenceSpace(session, &rsci, &view_space),
             "xrCreateReferenceSpace(VIEW)");

    /* wait for READY, then begin (view config type is ignored in headless) */
    bool running = false;
    fprintf(stderr, "Waiting for session...\n");
    while (!g_quit) {
        XrEventDataBuffer ev = {.type = XR_TYPE_EVENT_DATA_BUFFER};
        XrResult pr = xrPollEvent(instance, &ev);
        if (pr == XR_SUCCESS &&
            ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            XrEventDataSessionStateChanged *sc = (void *)&ev;
            if (sc->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo sbi = {
                    .type = XR_TYPE_SESSION_BEGIN_INFO,
                    .primaryViewConfigurationType =
                        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                };
                XR_CHECK(xrBeginSession(session, &sbi), "xrBeginSession");
                running = true;
                break;
            }
            if (sc->state == XR_SESSION_STATE_EXITING ||
                sc->state == XR_SESSION_STATE_LOSS_PENDING)
                goto cleanup;
        }
        usleep(10 * 1000);
    }
    if (!running) goto cleanup;
    fprintf(stderr, "Session running. Streaming to %s:%d at %.0f Hz. "
            "Enter = recenter, Ctrl-C = quit.\n",
            opt.host, opt.port, opt.rate_hz);

    vec3d origin_pos = {0};
    quatd origin_inv = {0, 0, 0, 1};
    const long period_ns = (long)(1e9 / opt.rate_hz);
    long n = 0;

    while (!g_quit) {
        /* keep the event queue drained */
        XrEventDataBuffer ev = {.type = XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(instance, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                XrEventDataSessionStateChanged *sc = (void *)&ev;
                if (sc->state == XR_SESSION_STATE_STOPPING ||
                    sc->state == XR_SESSION_STATE_EXITING ||
                    sc->state == XR_SESSION_STATE_LOSS_PENDING)
                    g_quit = 1;
            }
            ev.type = XR_TYPE_EVENT_DATA_BUFFER;
        }

        char c;
        while (read(STDIN_FILENO, &c, 1) == 1)
            if (c == '\n') g_recenter = 1;

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        XrTime xt;
        if (XR_FAILED(ts2time(instance, &now, &xt))) break;

        XrSpaceLocation loc = {.type = XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(xrLocateSpace(view_space, ref_space, xt, &loc)) &&
            (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {

            quatd q = {loc.pose.orientation.x, loc.pose.orientation.y,
                       loc.pose.orientation.z, loc.pose.orientation.w};
            vec3d p = {loc.pose.position.x, loc.pose.position.y,
                       loc.pose.position.z};

            if (g_recenter) {
                origin_pos = p;
                origin_inv = q_conj(q);
                g_recenter = 0;
                fprintf(stderr, "recentered\n");
            }

            /* pose relative to the recenter origin */
            vec3d dp = {p.x - origin_pos.x, p.y - origin_pos.y,
                        p.z - origin_pos.z};
            vec3d rp = q_rotate(origin_inv, dp);
            quatd rq = q_mul(origin_inv, q);

            double yaw, pitch, roll;
            q_to_ypr_deg(rq, &yaw, &pitch, &roll);

            double pkt[6] = {
                opt.ix * rp.x * opt.pos_scale,
                opt.iy * rp.y * opt.pos_scale,
                opt.iz * rp.z * opt.pos_scale,
                opt.iyaw * yaw,
                opt.ipitch * pitch,
                opt.iroll * roll,
            };
            sendto(sock, pkt, sizeof(pkt), 0,
                   (struct sockaddr *)&dst, sizeof(dst));

            if (opt.verbose && (n++ % (long)opt.rate_hz == 0))
                fprintf(stderr,
                        "pos %+7.2f %+7.2f %+7.2f cm  ypr %+7.2f %+7.2f %+7.2f deg\n",
                        pkt[0], pkt[1], pkt[2], pkt[3], pkt[4], pkt[5]);
        }

        struct timespec ts = {0, period_ns};
        nanosleep(&ts, NULL);
    }

cleanup:
    if (session != XR_NULL_HANDLE) {
        xrRequestExitSession(session);
        xrDestroySession(session);
    }
    if (instance != XR_NULL_HANDLE)
        xrDestroyInstance(instance);
    close(sock);
    fprintf(stderr, "bye\n");
    return 0;
}
