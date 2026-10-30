/* setup_inv.h -- what Allen knows about the computer he woke up on, for the
 * setup wizard (svc/setup_wizard.h): graphics cards, screens and their modes,
 * processors, memory, disks, USB devices, ACPI tables, the network around it
 * (other AienOS machines, the gateway) and the language models already there.
 *
 * Two sources fill one inventory:
 *   1. the machine itself: what this kernel finds (PCI, the screen mode the
 *      firmware set, the processor count, the disks); added with si_add_*;
 *   2. a host description, when AienOS runs inside a launcher (aienos-run):
 *      the launcher sees the real computer around the virtual one (its
 *      graphics card, its screens, its model folders, its running model
 *      servers) and hands that over as text, one item per line:
 *
 *        gpu vendor=10de device=2684 name="NVIDIA GeForce RTX 4090" vram_mib=24564 driver="NVIDIA 560.94"
 *        display name="Dell AW2723DF" w=2560 h=1440 hz=60 max_w=2560 max_h=1440 max_hz=240 vrr=1
 *        cpu name="AMD Ryzen 9 7950X" fast=16 efficient=0 threads=32
 *        ram mib=65536
 *        disk name="Samsung 990 PRO" gib=2000 kind=nvme
 *        usb name="Logitech G502 mouse"
 *        acpi tables="FACP APIC MCFG HPET"
 *        peer name="spark" kind=aienos addr=10.0.0.20 model="qwen3.8-27b-fp8" params_b10=270 url="http://10.0.0.20:18091"
 *        gateway addr=10.0.0.1
 *        model source=ollama name="llama3.1:8b" params_b10=80 bits10=48 mib=4700 path="/home/a/.ollama/models"
 *        server source=lmstudio name="qwen2.5-7b" url="http://127.0.0.1:1234"
 *        machine name="Games PC" from=host
 *
 *      Keys are words, values are bare words or "quoted" (no escapes; a
 *      quote cannot appear inside). Unknown item kinds and unknown keys are
 *      skipped and counted, so an older Allen reads a newer launcher's text.
 *      '#' starts a comment line. Nothing in it is trusted for safety: it
 *      only changes what Allen says and suggests.
 *
 * Freestanding: no libc, no heap, integers only. Host tested in
 * svc/tests/setup_wizard_test.c. */
#ifndef AIENOS_SETUP_INV_H
#define AIENOS_SETUP_INV_H
#include <stdint.h>

#define SI_NAME 48u
#define SI_URL 96u
#define SI_GPUS 4u
#define SI_DISPLAYS 4u
#define SI_DISKS 6u
#define SI_USB 8u
#define SI_PEERS 6u
#define SI_MODELS 8u

enum si_from { SI_FROM_HERE = 0, SI_FROM_HOST = 1 };

/* how a graphics card is driven */
enum si_gpu_kind { SI_GPU_UNKNOWN = 0, SI_GPU_DISCRETE, SI_GPU_INTEGRATED, SI_GPU_VIRTUAL, SI_GPU_UNIFIED };

typedef struct {
    uint16_t vendor, device;
    char name[SI_NAME];
    uint32_t vram_mib; /* its own memory; for a unified chip, what it may use */
    char driver[SI_NAME]; /* who drives it now ("" none, or the vendor's driver and version) */
    uint8_t kind, from;
} si_gpu;

typedef struct {
    char name[SI_NAME];
    uint16_t w, h, hz;          /* the mode it runs now (hz 0: not known) */
    uint16_t max_w, max_h, max_hz; /* the best mode it offers */
    uint8_t vrr, from;          /* variable refresh (G-Sync / FreeSync) */
} si_display;

typedef struct {
    char name[SI_NAME];
    uint16_t fast, efficient, threads; /* cores by class; efficient 0 for one class only */
    uint8_t from;
} si_cpu;

enum si_disk_kind { SI_DISK_UNKNOWN = 0, SI_DISK_NVME, SI_DISK_SSD, SI_DISK_HDD, SI_DISK_VIRTUAL };
typedef struct {
    char name[SI_NAME];
    uint32_t gib;
    uint8_t kind, from;
} si_disk;

enum si_peer_kind { SI_PEER_AIENOS = 1, SI_PEER_PRINTER, SI_PEER_NAS, SI_PEER_OTHER };
typedef struct {
    char name[SI_NAME];
    char addr[40];
    char model[SI_NAME];  /* the model it serves ("" none) */
    char url[SI_URL];     /* where it serves it */
    uint16_t params_b10;  /* that model's size, tenths of a billion parameters */
    uint8_t kind;
} si_peer;

/* where a language model was found */
enum si_model_src {
    SI_SRC_OLLAMA = 1,
    SI_SRC_LMSTUDIO,
    SI_SRC_LLAMACPP, /* a GGUF file on a disk */
    SI_SRC_SERVER,   /* something answering the OpenAI shape on this computer */
    SI_SRC_PEER,     /* another AienOS machine on the network */
    SI_SRC_PAIR,     /* Allen's own model pair (27B + 30B working together) */
};
typedef struct {
    char name[SI_NAME];
    char where[SI_URL]; /* a folder or a URL */
    uint16_t params_b10, bits10; /* 0: not known */
    uint32_t mib;                /* size on disk, 0: not known */
    uint8_t src, running;        /* running: answering now (a server) */
} si_model;

typedef struct {
    char machine[SI_NAME]; /* what the host calls itself ("" none) */
    int host;              /* a host description was read */
    si_gpu gpu[SI_GPUS];
    unsigned ngpu;
    si_display display[SI_DISPLAYS];
    unsigned ndisplay;
    si_cpu cpu; /* the processor (the host's when there is one, else this machine's) */
    uint32_t ram_mib;
    si_disk disk[SI_DISKS];
    unsigned ndisk;
    char usb[SI_USB][SI_NAME];
    unsigned nusb;
    unsigned npci;      /* PCI functions this kernel found */
    char acpi[64];      /* ACPI table signatures, space separated */
    char gateway[40];   /* "" not found */
    si_peer peer[SI_PEERS];
    unsigned npeer;
    si_model model[SI_MODELS];
    unsigned nmodel;
    unsigned skipped; /* host lines or keys not understood */
} si_inv;

void si_init(si_inv *v);

/* Reads a host description (n bytes, not NUL-terminated). Returns the number
 * of items taken; v->host is set when at least one was. */
unsigned si_parse(si_inv *v, const char *text, unsigned n);

/* What the kernel found itself (SI_FROM_HERE). Full lists keep the first. */
void si_add_gpu(si_inv *v, uint16_t vendor, uint16_t device, uint32_t pci_class);
void si_add_display(si_inv *v, uint16_t w, uint16_t h);
void si_add_cpus(si_inv *v, unsigned count, const char *name);
void si_add_disk(si_inv *v, const char *name, uint64_t bytes, uint8_t kind);
void si_add_ram(si_inv *v, uint64_t bytes);

/* PCI vendor id -> a plain name ("NVIDIA", "AMD", "Intel", ...; "a" when unknown). */
const char *si_vendor(uint16_t vendor);

/* The best graphics card for running a model: the most memory (-1: none). */
int si_best_gpu(const si_inv *v);

/* Plain-English lines saying what Allen found, at most max, each at most cap
 * bytes with its NUL, in the order of FIRST_RUN_UX screen 5: "Screen: ...",
 * "Graphics: NVIDIA GeForce RTX 4090, 24 GB of its own memory", ...
 * Returns the count. */
unsigned si_summary(const si_inv *v, char (*out)[96], unsigned max);

/* small freestanding text helpers shared by the setup files */
unsigned si_len(const char *s);
unsigned si_copy(char *d, const char *s, unsigned cap);
unsigned si_cat(char *d, unsigned at, const char *s, unsigned cap);
unsigned si_num(char *d, unsigned at, uint32_t v, unsigned cap);
#endif
