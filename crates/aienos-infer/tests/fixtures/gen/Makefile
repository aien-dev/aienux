# Rebuild the ggml golden fixtures: `make -C crates/aienos-infer/tests/fixtures/gen regen`
GGML_DIR ?= $(HOME)/.lmstudio/extensions/backends/llama.cpp-linux-arm64-2.33.0
MODEL ?= $(HOME)/models/aien-mail/Llama-3.2-1B-Instruct-Q4_K_M.gguf
gen: gen.c
	$(CC) -O1 gen.c -o gen -L$(GGML_DIR) -lggml-base -Wl,-rpath,$(GGML_DIR)
regen: gen
	./gen .. $(MODEL)
clean:
	rm -f gen
