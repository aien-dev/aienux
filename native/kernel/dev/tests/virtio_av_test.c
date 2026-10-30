/* virtio_av_test.c -- host test of the virtio-gpu and virtio-sound command
 * builders (dev/virtio_gpu.c, dev/virtio_snd.c) against the Linux UAPI copy
 * of the virtio spec structures (linux/virtio_gpu.h, linux/virtio_snd.h):
 * every constant, every size, every field offset written, and the reply
 * parsers. Run by scripts/ck_virtio_av_test.sh. */
#include "../virtio_gpu.h"
#include "../virtio_snd.h"
#include <linux/virtio_gpu.h>
#include <linux/virtio_snd.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int fails, checks;
#define CHECK(c, what)                                                                                           \
    do {                                                                                                         \
        checks++;                                                                                                \
        if (!(c)) {                                                                                              \
            fails++;                                                                                             \
            printf("FAIL %s\n", what);                                                                           \
        }                                                                                                        \
    } while (0)

_Static_assert(VGPU_CMD_GET_DISPLAY_INFO == VIRTIO_GPU_CMD_GET_DISPLAY_INFO, "gpu cmd");
_Static_assert(VGPU_CMD_RESOURCE_CREATE_2D == VIRTIO_GPU_CMD_RESOURCE_CREATE_2D, "gpu cmd");
_Static_assert(VGPU_CMD_SET_SCANOUT == VIRTIO_GPU_CMD_SET_SCANOUT, "gpu cmd");
_Static_assert(VGPU_CMD_RESOURCE_FLUSH == VIRTIO_GPU_CMD_RESOURCE_FLUSH, "gpu cmd");
_Static_assert(VGPU_CMD_TRANSFER_TO_HOST_2D == VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D, "gpu cmd");
_Static_assert(VGPU_CMD_RESOURCE_ATTACH_BACKING == VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING, "gpu cmd");
_Static_assert(VGPU_RESP_OK_NODATA == VIRTIO_GPU_RESP_OK_NODATA, "gpu resp");
_Static_assert(VGPU_RESP_OK_DISPLAY_INFO == VIRTIO_GPU_RESP_OK_DISPLAY_INFO, "gpu resp");
_Static_assert(VGPU_FORMAT_B8G8R8X8_UNORM == VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM, "gpu format");
_Static_assert(VGPU_MAX_SCANOUTS == VIRTIO_GPU_MAX_SCANOUTS, "gpu scanouts");
_Static_assert(VGPU_HDR_BYTES == sizeof(struct virtio_gpu_ctrl_hdr), "gpu hdr");
_Static_assert(VGPU_DISPLAY_INFO_BYTES == sizeof(struct virtio_gpu_resp_display_info), "gpu display info");
_Static_assert(VSND_R_PCM_INFO == VIRTIO_SND_R_PCM_INFO, "snd code");
_Static_assert(VSND_R_PCM_SET_PARAMS == VIRTIO_SND_R_PCM_SET_PARAMS, "snd code");
_Static_assert(VSND_R_PCM_PREPARE == VIRTIO_SND_R_PCM_PREPARE, "snd code");
_Static_assert(VSND_R_PCM_RELEASE == VIRTIO_SND_R_PCM_RELEASE, "snd code");
_Static_assert(VSND_R_PCM_START == VIRTIO_SND_R_PCM_START, "snd code");
_Static_assert(VSND_R_PCM_STOP == VIRTIO_SND_R_PCM_STOP, "snd code");
_Static_assert(VSND_S_OK == VIRTIO_SND_S_OK, "snd status");
_Static_assert(VSND_D_OUTPUT == VIRTIO_SND_D_OUTPUT, "snd direction");
_Static_assert(VSND_FMT_S16 == VIRTIO_SND_PCM_FMT_S16, "snd format");
_Static_assert(VSND_RATE_48000 == VIRTIO_SND_PCM_RATE_48000, "snd rate");
_Static_assert(VSND_VQ_CONTROL == VIRTIO_SND_VQ_CONTROL && VSND_VQ_TX == VIRTIO_SND_VQ_TX, "snd queues");
_Static_assert(VSND_INFO_BYTES == sizeof(struct virtio_snd_pcm_info), "snd info");

int main(void)
{
    uint8_t b[512];
    struct virtio_gpu_resource_create_2d c2;
    CHECK(vgpu_cmd_create_2d(b, sizeof b, 1, 2, 1280, 800) == sizeof c2, "create_2d size");
    memcpy(&c2, b, sizeof c2);
    CHECK(c2.hdr.type == VIRTIO_GPU_CMD_RESOURCE_CREATE_2D && c2.resource_id == 1 && c2.format == 2 &&
              c2.width == 1280 && c2.height == 800 && c2.hdr.flags == 0 && c2.hdr.fence_id == 0,
          "create_2d fields");

    struct {
        struct virtio_gpu_resource_attach_backing a;
        struct virtio_gpu_mem_entry e;
    } ab;
    CHECK(vgpu_cmd_attach_backing(b, sizeof b, 1, 0x40001000ull, 4096000) == sizeof ab, "attach size");
    memcpy(&ab, b, sizeof ab);
    CHECK(ab.a.hdr.type == VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING && ab.a.resource_id == 1 && ab.a.nr_entries == 1 &&
              ab.e.addr == 0x40001000ull && ab.e.length == 4096000 && ab.e.padding == 0,
          "attach fields");

    struct virtio_gpu_set_scanout ss;
    CHECK(vgpu_cmd_set_scanout(b, sizeof b, 0, 1, 1280, 800) == sizeof ss, "scanout size");
    memcpy(&ss, b, sizeof ss);
    CHECK(ss.hdr.type == VIRTIO_GPU_CMD_SET_SCANOUT && ss.r.x == 0 && ss.r.y == 0 && ss.r.width == 1280 &&
              ss.r.height == 800 && ss.scanout_id == 0 && ss.resource_id == 1,
          "scanout fields");

    struct virtio_gpu_transfer_to_host_2d t;
    CHECK(vgpu_cmd_transfer_2d(b, sizeof b, 1, 2, 3, 4, 5, 0x1234) == sizeof t, "transfer size");
    memcpy(&t, b, sizeof t);
    CHECK(t.hdr.type == VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D && t.r.x == 2 && t.r.y == 3 && t.r.width == 4 &&
              t.r.height == 5 && t.offset == 0x1234 && t.resource_id == 1 && t.padding == 0,
          "transfer fields");

    struct virtio_gpu_resource_flush fl;
    CHECK(vgpu_cmd_flush(b, sizeof b, 1, 0, 0, 1280, 800) == sizeof fl, "flush size");
    memcpy(&fl, b, sizeof fl);
    CHECK(fl.hdr.type == VIRTIO_GPU_CMD_RESOURCE_FLUSH && fl.r.width == 1280 && fl.r.height == 800 &&
              fl.resource_id == 1,
          "flush fields");
    CHECK(vgpu_cmd_display_info(b, sizeof b) == sizeof(struct virtio_gpu_ctrl_hdr), "display info cmd");
    CHECK(vgpu_cmd_flush(b, 10, 1, 0, 0, 1, 1) == 0, "short buffer refused");

    struct virtio_gpu_resp_display_info di;
    memset(&di, 0, sizeof di);
    di.hdr.type = VIRTIO_GPU_RESP_OK_DISPLAY_INFO;
    di.pmodes[0].r.width = 1280, di.pmodes[0].r.height = 800, di.pmodes[0].enabled = 1;
    uint32_t w = 0, h = 0;
    CHECK(vgpu_display0((const uint8_t *)&di, sizeof di, &w, &h) == 1 && w == 1280 && h == 800, "display0");
    di.pmodes[0].enabled = 0;
    CHECK(vgpu_display0((const uint8_t *)&di, sizeof di, &w, &h) == 0, "display0 disabled");
    di.pmodes[0].enabled = 1, di.hdr.type = VIRTIO_GPU_RESP_ERR_UNSPEC;
    CHECK(vgpu_display0((const uint8_t *)&di, sizeof di, &w, &h) == 0, "display0 error reply");
    CHECK(vgpu_display0((const uint8_t *)&di, 30, &w, &h) == 0, "display0 short reply");

    struct virtio_snd_query_info q;
    CHECK(vsnd_cmd_query(b, sizeof b, VSND_R_PCM_INFO, 0, 3, VSND_INFO_BYTES) == sizeof q, "query size");
    memcpy(&q, b, sizeof q);
    CHECK(q.hdr.code == VIRTIO_SND_R_PCM_INFO && q.start_id == 0 && q.count == 3 &&
              q.size == sizeof(struct virtio_snd_pcm_info),
          "query fields");

    struct virtio_snd_pcm_set_params sp;
    CHECK(vsnd_cmd_set_params(b, sizeof b, 1, 76800, 19200, 2, VSND_FMT_S16, VSND_RATE_48000) == sizeof sp,
          "set_params size");
    memcpy(&sp, b, sizeof sp);
    CHECK(sp.hdr.hdr.code == VIRTIO_SND_R_PCM_SET_PARAMS && sp.hdr.stream_id == 1 && sp.buffer_bytes == 76800 &&
              sp.period_bytes == 19200 && sp.features == 0 && sp.channels == 2 &&
              sp.format == VIRTIO_SND_PCM_FMT_S16 && sp.rate == VIRTIO_SND_PCM_RATE_48000 && sp.padding == 0,
          "set_params fields");

    struct virtio_snd_pcm_hdr ph;
    CHECK(vsnd_cmd_pcm(b, sizeof b, VSND_R_PCM_START, 2) == sizeof ph, "pcm hdr size");
    memcpy(&ph, b, sizeof ph);
    CHECK(ph.hdr.code == VIRTIO_SND_R_PCM_START && ph.stream_id == 2, "pcm hdr fields");

    /* on the wire the infos follow the 4-byte status directly (no padding to 8) */
    struct {
        struct virtio_snd_hdr st;
        struct virtio_snd_pcm_info in[3];
    } r;
    uint8_t wire[4 + 3 * sizeof(struct virtio_snd_pcm_info)];
#define WIRE() (memcpy(wire, &r.st, 4), memcpy(wire + 4, r.in, sizeof r.in), wire)
    memset(&r, 0, sizeof r);
    r.st.code = VIRTIO_SND_S_OK;
    r.in[0].direction = VIRTIO_SND_D_INPUT; /* a microphone: skipped */
    r.in[0].formats = 1u << VIRTIO_SND_PCM_FMT_S16, r.in[0].rates = 1u << VIRTIO_SND_PCM_RATE_48000;
    r.in[0].channels_min = 1, r.in[0].channels_max = 2;
    r.in[1] = r.in[0], r.in[1].direction = VIRTIO_SND_D_OUTPUT, r.in[1].rates = 1u << VIRTIO_SND_PCM_RATE_44100;
    r.in[2] = r.in[1], r.in[2].rates |= 1u << VIRTIO_SND_PCM_RATE_48000;
    CHECK(vsnd_pick_output(WIRE(), sizeof wire, 3) == 2, "pick the 48 kHz stereo output");
    r.in[2].channels_max = 1;
    CHECK(vsnd_pick_output(WIRE(), sizeof wire, 3) == -1, "mono only: none");
    r.in[2].channels_max = 2, r.st.code = VIRTIO_SND_S_BAD_MSG;
    CHECK(vsnd_pick_output(WIRE(), sizeof wire, 3) == -1, "error reply: none");
    r.st.code = VIRTIO_SND_S_OK;
    CHECK(vsnd_pick_output(WIRE(), 4 + 2 * sizeof(struct virtio_snd_pcm_info), 3) == -1,
          "short reply: the third entry is not read");

    printf("VIRTIO_AV_TEST: %s (%d checks + %d layout asserts at build)\n", fails ? "FAIL" : "PASS", checks, 25);
    return fails ? 1 : 0;
}
