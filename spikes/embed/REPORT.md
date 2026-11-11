# Spike: bge-m3 embeddings on the local GPU (Stage 0)

Date: 2026-11-12 · GPU: RTX 3080 Ti 12 GB (sm_86), driver 580, CUDA 12.8, cuDNN 9.10 · Model: `BAAI/bge-m3` `onnx/` (fp32, 2.27 GB) · Scripts: `spike.py`, `convert_fp16.py`, `mem_probe.py`.

## Results

| Check | Result |
|---|---|
| Graph | Inputs `input_ids`, `attention_mask` (int64, dynamic batch × seq). Outputs `token_embeddings` and **`sentence_embedding` (already CLS-pooled and L2-normalized; cosine 1.0 vs manual CLS)**, so no pooling code is needed in C++ |
| Tokenizer (risk 6) | **sentencepiece + XLM-R fairseq mapping** (`id = sp_id + 1`, `sp_id 0 → <unk>=3`, wrap `<s>=0 … </s>=2`, pad=1) matches the reference HF `tokenizer.json` on 14/15 raw cases (Tamil, Hindi, Japanese, emoji, code, SQL, ligatures, full-width, 3000-char run, long repeated text). The single mismatch is trailing whitespace. **After trimming and collapsing whitespace before tokenizing: 15/15.** The C++ backend uses the vcpkg `sentencepiece` port with this mapping and normalization; no Rust/HF tokenizers dependency is needed |
| Semantic sanity | Related pairs above unrelated for every pair, including cross-lingual Tamil ↔ English (0.94 vs 0.51) |
| GPU vs CPU parity | min cosine 1.000000, max abs diff 9e-5 |
| **fp16 (attention-fused)** | min cosine 0.99999 vs fp32, top-5 neighbours identical 26/26, pair similarities equal to 3 decimals |

### Performance (sentence_embedding, CUDA EP)

| | seq 64 ×1 | seq 64 ×32 | seq 512 ×1 | seq 512 ×32 |
|---|---|---|---|---|
| fp32 | 4.0 ms | 57 ms (566/s) | 19.5 ms | 506 ms (63/s) |
| **fp16 fused** | **2.1 ms** | **24 ms (1311/s)** | **8.2 ms** | **191 ms (168/s)** |
| CPU fp32 (accidental fallback, see below) | 66 ms | 1355 ms | 394 ms | 13.7 s |

### GPU memory (per process, `arena_extend_strategy = kSameAsRequested`)

| Model | max batch 8 @ 512 | max batch 32 @ 512 |
|---|---|---|
| fp32 | 2.8 GB | 3.8 GB |
| **fp16 fused** | **1.5 GB** | **2.0 GB** |

The spec's "about 1 GB" estimate is low. The realistic budget is **~1.5 GB with fp16 and batches ≤ 8**, still well inside the 12 GB card.

## Traps found

1. **Silent CPU fallback.** `onnxruntime-gpu` ≥ 1.27 is built for **CUDA 13** (`libcublasLt.so.13`). On this CUDA 12.8 machine the CUDA provider failed to load, ORT logged an error and **ran on CPU without failing**, 15–30× slower. **1.26.0 is the last CUDA 12 build.** The backend must check that `CUDAExecutionProvider` is active after session creation and refuse to start the embedder otherwise (or start in an explicit, logged `cpu` mode only when configured).
2. **Naive fp16 conversion breaks the graph** (`onnxconverter_common` leaves a float/float16 mismatch in the attention-mask `Sub`). Use the ORT transformers optimizer (`convert_fp16.py`), which also fuses 24 attention blocks.
3. **>2 GB protobuf limit:** shape inference cannot serialize the fp32 model; conversion must keep external data.

## Recommendations for Stage 2 (`agent_lld.md` §6, `backend_lld.md`)

1. **Model artifact:** fp16 fused bge-m3, produced once by `convert_fp16.py`. Stored vectors carry `model = "bge-m3"`, `model_version = "onnx-fp16-fused@ort1.26"`, so a change triggers re-embedding (spec 5.4).
2. **Runtime:** the official ONNX Runtime **1.26.0 linux-x64-gpu (CUDA 12) prebuilt release**, fetched by CMake with a pinned SHA-256. vcpkg's `onnxruntime[cuda]` port (1.31.1) builds ORT and CUDA dependencies from source, which takes hours and would target CUDA 13-era code paths.
3. **Process boundary (recommended): a separate `thelife-embedder` process** on a Unix socket, rather than in-process in the API server:
   - a CUDA fault or out-of-memory error cannot take down the API (same reasoning as Stage 6)
   - the backend binary and its CI stay free of CUDA libraries; tests use a fake embedder
   - GPU memory is owned by one process with fixed limits (`gpu_mem_limit` ≈ 2.5 GB, `kSameAsRequested`, max batch 8, max seq 512 for chunks)
4. **Chunking:** embed chunks of ≤ 512 tokens. At 8 ms per chunk (fp16), a day of messages, activities and summaries embeds in seconds.
