/* virtio_snd.h -- virtio-sound PCM output (virtio 1.x section 5.14), polled,
 * for the boot sequence's chirp in a VM (QEMU virtio-sound-pci, the
 * aienos-run window's speakers). One output stream, 2 channels, S16, 48 kHz:
 * the chirp's own format (ui/chirp.h), so the samples go out as made. The
 * stream is started just before the chirp and fed from the night's clock
 * (ui/boot_show.h), so the sound lands on the merger. Layouts follow the
 * spec (checked against the Linux UAPI copy, linux/virtio_snd.h, in the host
 * test); builders are pure, the device goes through virtio_q.h. */
#ifndef AIENOS_CK_VIRTIO_SND_H
#define AIENOS_CK_VIRTIO_SND_H
#include <stddef.h>
#include <stdint.h>
#include "virtio_q.h"

#define VSND_R_PCM_INFO 0x0100u
#define VSND_R_PCM_SET_PARAMS 0x0101u
#define VSND_R_PCM_PREPARE 0x0102u
#define VSND_R_PCM_RELEASE 0x0103u
#define VSND_R_PCM_START 0x0104u
#define VSND_R_PCM_STOP 0x0105u
#define VSND_S_OK 0x8000u
#define VSND_D_OUTPUT 0u
#define VSND_FMT_S16 5u
#define VSND_RATE_48000 7u
#define VSND_VQ_CONTROL 0u
#define VSND_VQ_TX 2u
#define VSND_INFO_BYTES 32u /* struct virtio_snd_pcm_info */

size_t vsnd_cmd_query(uint8_t *out, size_t cap, uint32_t code, uint32_t start, uint32_t count, uint32_t size);
size_t vsnd_cmd_set_params(uint8_t *out, size_t cap, uint32_t stream, uint32_t buffer_bytes, uint32_t period_bytes,
                           uint8_t channels, uint8_t format, uint8_t rate);
/* PREPARE, RELEASE, START, STOP: struct virtio_snd_pcm_hdr */
size_t vsnd_cmd_pcm(uint8_t *out, size_t cap, uint32_t code, uint32_t stream);
/* The first output stream in a PCM_INFO reply of n entries that takes
 * stereo S16 at 48 kHz; -1 if none. */
int vsnd_pick_output(const uint8_t *resp, size_t len, uint32_t n);

#ifndef CK_HOSTED
/* Kernel side. DMA bytes the driver needs after the rings (command, reply,
 * four 100 ms periods). */
#define VSND_EXTRA_BYTES (2u * 4096u + 4u * 20480u)
/* Prepares the stream on a device the caller brought up with 3 queues
 * (control, event, tx), before DRIVER_OK; mem/bus = VSND_EXTRA_BYTES after the
 * rings. 0 and a "virtio_snd: ..." line, or < 0 with a line saying why not. */
int ck_vsnd_attach(vq_dev *dev, uint8_t *mem, uint64_t bus);
#ifndef CK_X86
#include "pci.h"
/* aarch64: finds the virtio-sound PCI function, confines its DMA (virtio_bind.h)
 * and attaches. No device: < 0, and the night plays silent. */
int ck_vsnd_open(const pci_system *pci);
#endif
/* Called every turn of the boot loop with the night's clock: starts the
 * stream just before the chirp, keeps it fed, stops it after. Returns 1
 * while it still has work, 0 when done or off. */
int ck_vsnd_step(uint32_t night_ms, uint32_t audio_frame);
#endif
#endif
