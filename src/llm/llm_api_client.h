#pragma once

#include <aevos/types.h>
#include <aevos/llm_syscall.h>

/* OpenAI-compatible chat completion over TCP (stub: returns -ENOTSUP until TLS/HTTP done). */
int llm_remote_infer(const llm_infer_request_t *req, char *out, size_t out_max);
