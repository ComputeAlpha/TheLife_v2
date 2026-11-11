#!/usr/bin/env python3
"""Stage 0 spike: bge-m3 dense embeddings on the local GPU through ONNX Runtime (risk 6 and 4.5 of the spec).

Checks:
  1. ONNX graph inputs/outputs.
  2. Tokenizer parity: sentencepiece + XLM-R fairseq id mapping (what the C++ backend will implement with the
     sentencepiece library) versus the reference HF tokenizer.json, over a mixed corpus.
  3. CUDA EP vs CPU EP numerical parity of the pooled embeddings.
  4. Semantic sanity: paraphrases score above unrelated pairs.
  5. Throughput and latency for batch sizes and sequence lengths; GPU memory used.

Setup: python3 -m venv .venv && .venv/bin/pip install onnxruntime-gpu==1.26.0 numpy   # last CUDA 12 build sentencepiece tokenizers huggingface_hub
Model: huggingface BAAI/bge-m3, onnx/* downloaded into models/bge-m3 (git-ignored).
"""
from __future__ import annotations

import json
import pathlib
import subprocess
import time

import numpy as np
import onnxruntime as ort
import sentencepiece as spm
from tokenizers import Tokenizer

HERE = pathlib.Path(__file__).parent
MODEL_DIR = HERE / "models" / "bge-m3" / "onnx"
OUT = HERE / "out"

CORPUS = [
    "health.cc — TheLife_v2 — Visual Studio Code",
    "Lecture 7: Eigenvalues and Eigenvectors — MIT 18.06 Linear Algebra - Chromium",
    "Did 45 minutes of the Rust course, section 4, then 30 min at the gym.",
    "Remind me at 6pm to call the bank about the credit card statement.",
    "நான் இன்று காலை 5 கிமீ ஓடினேன்",                 # Tamil
    "आज मैंने किताब के 30 पन्ने पढ़े",                    # Hindi
    "今日は日本語の勉強を一時間しました。",                 # Japanese
    "Emoji test 🚀🔥 — done ✅ (v2.0) & <tags> \"quotes\" 'single'",
    "    leading and trailing spaces    ",
    "co_return HttpResponse::newHttpJsonResponse(body); // C++20",
    "SELECT coalesce(max(version), '') AS v FROM schema_migrations;",
    "ÀÉÎÕÜ àéîõü ß Œ œ ﬁ ligature ｆｕｌｌｗｉｄｔｈ",
    "",
    "a" * 3000,
    " ".join(["The weekly review covers plans, goals, detections and disputes."] * 60),
]


def normalize_ws(text: str) -> str:
    """Trim and collapse whitespace runs to one space. Applied before tokenizing in the backend, it makes
    sentencepiece and the HF reference tokenizer agree (they differ on trailing whitespace)."""
    return " ".join(text.split())


class SpTokenizer:
    """XLM-RoBERTa tokenization as the C++ code will do it: sentencepiece + fairseq offset."""
    BOS, PAD, EOS, UNK = 0, 1, 2, 3
    OFFSET = 1

    def __init__(self, path: pathlib.Path):
        self.sp = spm.SentencePieceProcessor(model_file=str(path))

    def encode(self, text: str, max_len: int = 8192) -> list[int]:
        ids = []
        for sp_id in self.sp.encode(text, out_type=int):
            ids.append(self.UNK if sp_id == 0 else sp_id + self.OFFSET)
        ids = ids[: max_len - 2]
        return [self.BOS] + ids + [self.EOS]


def pad_batch(batch: list[list[int]]):
    n = max(len(x) for x in batch)
    ids = np.full((len(batch), n), SpTokenizer.PAD, dtype=np.int64)
    mask = np.zeros((len(batch), n), dtype=np.int64)
    for i, x in enumerate(batch):
        ids[i, : len(x)] = x
        mask[i, : len(x)] = 1
    return ids, mask


def embed(sess: ort.InferenceSession, out_name: str, batch: list[list[int]]) -> np.ndarray:
    ids, mask = pad_batch(batch)
    feeds = {"input_ids": ids, "attention_mask": mask}
    hidden = sess.run([out_name], feeds)[0]
    vec = hidden[:, 0, :] if hidden.ndim == 3 else hidden  # CLS pooling (1_Pooling/config.json)
    return vec / np.linalg.norm(vec, axis=1, keepdims=True)


def gpu_mem_mib() -> int:
    out = subprocess.run(["nvidia-smi", "--query-gpu=memory.used", "--format=csv,noheader,nounits"],
                         capture_output=True, text=True).stdout.strip().splitlines()
    return int(out[0]) if out else -1


def main():
    OUT.mkdir(exist_ok=True)
    report: dict = {}

    # 2. tokenizer parity
    hf = Tokenizer.from_file(str(MODEL_DIR / "tokenizer.json"))
    sp = SpTokenizer(MODEL_DIR / "sentencepiece.bpe.model")
    mismatches = []
    for text in CORPUS:
        a = hf.encode(text).ids
        b = sp.encode(text)
        if a != b:
            first = next((i for i, (x, y) in enumerate(zip(a, b)) if x != y), min(len(a), len(b)))
            mismatches.append({"text": text[:60], "hf_len": len(a), "sp_len": len(b), "first_diff": first,
                               "hf": a[max(0, first - 2): first + 3], "sp": b[max(0, first - 2): first + 3]})
    normalized_mismatch = [t[:40] for t in CORPUS
                           if hf.encode(normalize_ws(t)).ids != sp.encode(normalize_ws(t))]
    report["tokenizer_parity"] = {"cases": len(CORPUS), "mismatches_raw": mismatches,
                                  "mismatches_after_ws_normalization": normalized_mismatch}
    print(f"[tokenizer] raw: {len(CORPUS) - len(mismatches)}/{len(CORPUS)} identical; "
          f"after whitespace normalization: {len(CORPUS) - len(normalized_mismatch)}/{len(CORPUS)}")
    for m in mismatches:
        print("   raw mismatch:", m)

    # 1. graph
    gpu_before = gpu_mem_mib()
    t0 = time.monotonic()
    cuda = ort.InferenceSession(str(MODEL_DIR / "model.onnx"),
                                providers=[("CUDAExecutionProvider", {"device_id": 0}), "CPUExecutionProvider"])
    load_s = time.monotonic() - t0
    if "CUDAExecutionProvider" not in cuda.get_providers():
        raise SystemExit("CUDA execution provider not active (ONNX Runtime fell back to CPU). Check CUDA major "
                         "version of the onnxruntime build against the installed toolkit.")
    ins = [(i.name, i.type, i.shape) for i in cuda.get_inputs()]
    outs = [(o.name, o.type, o.shape) for o in cuda.get_outputs()]
    out_name = cuda.get_outputs()[0].name  # token_embeddings; CLS pooled below
    ids, mask = pad_batch([sp.encode(t) for t in CORPUS[:4]])
    tok, sent = cuda.run(["token_embeddings", "sentence_embedding"], {"input_ids": ids, "attention_mask": mask})
    cls = tok[:, 0, :] / np.linalg.norm(tok[:, 0, :], axis=1, keepdims=True)
    report["sentence_embedding_output"] = {
        "norms": [round(float(x), 4) for x in np.linalg.norm(sent, axis=1)],
        "cos_vs_normalized_cls": [round(float(x), 6) for x in np.sum(cls * sent / np.linalg.norm(sent, axis=1, keepdims=True), axis=1)]}
    print(f"[graph] sentence_embedding norms={report['sentence_embedding_output']['norms']} "
          f"cos(vs CLS)={report['sentence_embedding_output']['cos_vs_normalized_cls']}")
    report["graph"] = {"inputs": ins, "outputs": outs, "providers": cuda.get_providers(), "load_s": round(load_s, 2)}
    print(f"[graph] inputs={ins}\n        outputs={outs}\n        providers={cuda.get_providers()} load={load_s:.1f}s")

    # 3. parity CPU vs GPU
    sample = [sp.encode(t) for t in CORPUS[:12]]
    g = embed(cuda, out_name, sample)
    cpu = ort.InferenceSession(str(MODEL_DIR / "model.onnx"), providers=["CPUExecutionProvider"])
    c = embed(cpu, out_name, sample)
    cos = np.sum(g * c, axis=1)
    report["cpu_gpu_parity"] = {"min_cos": float(cos.min()), "dim": int(g.shape[1])}
    print(f"[parity] dim={g.shape[1]} min cosine(GPU,CPU)={cos.min():.6f} max abs diff={np.abs(g - c).max():.2e}")
    del cpu

    # 4. semantic sanity
    pairs = [
        ("I went for a 5 km run this morning", "Did a morning jog of five kilometres", True),
        ("I went for a 5 km run this morning", "Fixed the database migration bug", False),
        ("Read 30 pages of Deep Work", "Finished three chapters of the productivity book", True),
        ("Read 30 pages of Deep Work", "Played two rounds of a shooter game", False),
        ("நான் இன்று காலை 5 கிமீ ஓடினேன்", "I ran 5 km this morning", True),
        ("நான் இன்று காலை 5 கிமீ ஓடினேன்", "Called the bank about my statement", False),
    ]
    sims = []
    for a, b, related in pairs:
        e = embed(cuda, out_name, [sp.encode(a), sp.encode(b)])
        sims.append((float(e[0] @ e[1]), related, a[:30], b[:30]))
    ok = all(s_rel > s_unrel for (s_rel, r1, *_), (s_unrel, r2, *_) in zip(sims[0::2], sims[1::2]))
    report["semantic"] = {"ok": ok, "pairs": sims}
    print(f"[semantic] related>unrelated for every pair: {ok}")
    for s, r, a, b in sims:
        print(f"    {s:.3f} {'REL ' if r else 'UNR '} {a!r} ~ {b!r}")

    # 5. performance
    perf = []
    text = " ".join(["Worked on the backend health checks and readiness probes for the service."] * 40)
    base = sp.encode(text)
    for seq in (64, 256, 512):
        ids = base[: seq - 1] + [SpTokenizer.EOS]
        for bs in (1, 16, 32):
            batch = [ids] * bs
            embed(cuda, out_name, batch)  # warm-up
            n = 5
            t0 = time.monotonic()
            for _ in range(n):
                embed(cuda, out_name, batch)
            dt = (time.monotonic() - t0) / n
            perf.append({"seq": seq, "batch": bs, "ms_per_batch": round(dt * 1000, 1),
                         "texts_per_s": round(bs / dt, 1)})
            print(f"[perf] seq={seq:4} batch={bs:3}  {dt * 1000:7.1f} ms/batch  {bs / dt:7.1f} texts/s")
    report["perf"] = perf
    report["gpu_mem_mib"] = {"before": gpu_before, "after": gpu_mem_mib()}
    print(f"[gpu] memory used before={gpu_before} MiB after={gpu_mem_mib()} MiB (whole GPU, includes other processes)")

    (OUT / "results.json").write_text(json.dumps(report, indent=2, ensure_ascii=False, default=str))


if __name__ == "__main__":
    main()
