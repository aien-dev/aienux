/* Immutable inputs covered by the owner-signed model artifact and boot image. */
.section .rodata,"a",@progbits
.balign 4096
.global ck_model_weights, ck_model_weights_end
ck_model_weights:
.incbin CK_MODEL_FILE
ck_model_weights_end:
/* Never expose adjacent tokenizer bytes through the final weights page. */
.balign 4096, 0
.global ck_model_tokenizer, ck_model_tokenizer_end
ck_model_tokenizer:
.incbin CK_TOKENIZER_FILE
ck_model_tokenizer_end:
.section .note.GNU-stack,"",@progbits
