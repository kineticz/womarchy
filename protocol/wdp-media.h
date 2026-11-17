/*
 * WDP media channel: audio and cameras between womarchy-mediad (in the session) and the Windows viewer
 * (omarchy.exe). A companion to wdp.h, which defines the framing, the handshake pattern and
 * WDP_VERSION / WDP_PROOF_BYTES; it is a separate file because the compositor (aquamarine) carries a
 * copy of wdp.h and has no use for these messages.
 *
 * Channel: womarchy-mediad listens on the compositor's port + WDP_MEDIA_PORT_OFFSET with its own
 * 32-byte token (WOMARCHY_MEDIA_TOKEN) and the same handshake (WDP_MEDIA_HELLO / WDP_MEDIA_WELCOME;
 * the hello also lists what the viewer can do). It offers Linux a sink and a source backed by Windows'
 * default devices, and drives both streams; the viewer only follows:
 *   playback  mediad -> viewer: AUDIO_PLAY_START (format), AUDIO_PLAY_DATA..., AUDIO_PLAY_STOP when
 *             nothing plays any more. The viewer plays the data (WASAPI) and drains it after STOP.
 *   capture   mediad -> viewer: AUDIO_CAPTURE_START (format) when a Linux app starts recording, then
 *             AUDIO_CAPTURE_DATA... flows viewer -> mediad until AUDIO_CAPTURE_STOP. The viewer opens
 *             the microphone only in between. AUDIO_CAPTURE_FAILED reports why it could not.
 * PCM is interleaved little-endian float32 (WDP_AUDIO_F32LE), at the rate and channel count of the
 * START message; each DATA message holds whole frames. Each side corrects clock drift on its receiving
 * end. (Added without a version bump: a viewer without it never connects, and the session then keeps
 * WSLg's audio.)
 *
 * Cameras (same channel, viewer with WDP_MEDIA_CAP_CAMERA): right after the handshake the viewer sends
 * CAMERA_LIST, the cameras and the modes it can deliver (NV12, at most 1080p). mediad offers each as a
 * video source and drives it like the microphone: CAMERA_START (camera, mode) when an app starts
 * streaming, CAMERA_FRAMEs from the viewer, CAMERA_STOP when no app streams any more. The camera is on
 * only in between. Each frame is the NV12 image packed: width*height luma bytes, then height/2 rows of
 * width interleaved U,V bytes.
 */
#ifndef WDP_MEDIA_H
#define WDP_MEDIA_H
#include "wdp.h"

#define WDP_MEDIA_PORT_OFFSET 2u

#define WDP_MAX_AUDIO_DATA (1u << 20)  /* bytes per media channel message */
#define WDP_MAX_AUDIO_RATE 384000u
#define WDP_MAX_AUDIO_CHANNELS 8u
#define WDP_MAX_CAMERAS        8u
#define WDP_MAX_CAMERA_MODES   32u          /* per camera */
#define WDP_MAX_CAMERA_WIDTH   1920u
#define WDP_MAX_CAMERA_HEIGHT  1080u
#define WDP_MAX_CAMERA_FRAME   (8u << 20)   /* CAMERA_FRAME payload bytes */

/* message types: media channel */
#define WDP_MEDIA_HELLO          300u /* viewer -> mediad: wdp_media_hello */
#define WDP_MEDIA_WELCOME        301u /* mediad -> viewer: wdp_media_welcome, proves mediad */
#define WDP_AUDIO_PLAY_START     310u /* mediad -> viewer: wdp_audio_format; open the output device */
#define WDP_AUDIO_PLAY_DATA      311u /* mediad -> viewer: PCM frames to play */
#define WDP_AUDIO_PLAY_STOP      312u /* mediad -> viewer: no payload; play out what is queued, then close */
#define WDP_AUDIO_CAPTURE_START  320u /* mediad -> viewer: wdp_audio_format; open the microphone */
#define WDP_AUDIO_CAPTURE_DATA   321u /* viewer -> mediad: recorded PCM frames */
#define WDP_AUDIO_CAPTURE_STOP   322u /* mediad -> viewer: no payload; close the microphone */
#define WDP_AUDIO_CAPTURE_FAILED 323u /* viewer -> mediad: wdp_audio_failed */
#define WDP_CAMERA_LIST          330u /* viewer -> mediad: u32 count, then per camera wdp_camera +
                                         wdp_camera_mode[nmodes] */
#define WDP_CAMERA_START         331u /* mediad -> viewer: wdp_camera_start; turn the camera on */
#define WDP_CAMERA_FRAME         332u /* viewer -> mediad: wdp_camera_frame + NV12 pixels */
#define WDP_CAMERA_STOP          333u /* mediad -> viewer: wdp_camera_stop; turn it off */
#define WDP_CAMERA_FAILED        334u /* viewer -> mediad: wdp_camera_failed */

/* capabilities (wdp_media_hello.caps) and sample formats (wdp_audio_format.format) */
#define WDP_MEDIA_CAP_PLAYBACK (1u << 0)
#define WDP_MEDIA_CAP_CAPTURE  (1u << 1)
#define WDP_MEDIA_CAP_CAMERA   (1u << 2)
#define WDP_AUDIO_F32LE        1u
#define WDP_CAMERA_NV12        1u

#pragma pack(push, 1)

struct wdp_media_hello {
    uint32_t version;                /* WDP_VERSION */
    uint8_t  proof[WDP_PROOF_BYTES]; /* media token[0..16) */
    uint32_t caps;                   /* WDP_MEDIA_CAP_*: mediad offers only the devices listed here */
};

struct wdp_media_welcome {
    uint32_t version;
    uint8_t  proof[WDP_PROOF_BYTES]; /* media token[16..32) */
};

struct wdp_audio_format {
    uint32_t rate;     /* frames per second, 8000..WDP_MAX_AUDIO_RATE */
    uint32_t channels; /* 1..WDP_MAX_AUDIO_CHANNELS; 2 = front left, front right */
    uint32_t format;   /* WDP_AUDIO_F32LE */
};

struct wdp_audio_failed {
    int32_t hresult;   /* e.g. E_ACCESSDENIED: Windows' microphone privacy setting denies desktop apps */
};

struct wdp_camera_mode {
    uint32_t width, height; /* even, <= WDP_MAX_CAMERA_WIDTH x WDP_MAX_CAMERA_HEIGHT */
    uint32_t fps_num, fps_den;
};

struct wdp_camera {
    uint32_t id;       /* the viewer's number for it, for this connection */
    char     name[64]; /* UTF-8, NUL padded, e.g. "Integrated Webcam" */
    uint32_t nmodes;   /* <= WDP_MAX_CAMERA_MODES; the first is the one to offer first */
    /* struct wdp_camera_mode modes[nmodes]; */
};

struct wdp_camera_start {
    uint32_t id;
    struct wdp_camera_mode mode; /* one of the listed modes */
    uint32_t format;             /* WDP_CAMERA_NV12 */
};

struct wdp_camera_frame {
    uint32_t id;
    uint32_t width, height; /* the started mode's */
    uint32_t format;        /* WDP_CAMERA_NV12 */
    uint64_t timestamp_ns;  /* capture time, from the start of the stream */
    /* uint8_t nv12[width * height * 3 / 2]; */
};

struct wdp_camera_stop {
    uint32_t id;
};

struct wdp_camera_failed {
    uint32_t id;
    int32_t  hresult; /* e.g. E_ACCESSDENIED (camera privacy setting), or the camera is in use */
};

#pragma pack(pop)
#endif
