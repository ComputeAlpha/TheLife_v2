"""Reference token ids and embeddings for the embedder parity test (backend/tests/embedder/embedder_test.cc).
Uses the upstream stack: the Hugging Face tokenizer.json and the fp32 bge-m3 ONNX model on the CPU.

  python3 -m venv build/embed-ref-venv && build/embed-ref-venv/bin/pip install onnxruntime==1.26.0 tokenizers numpy
  build/embed-ref-venv/bin/python backend/tests/fixtures/embed/make_reference.py spikes/embed/models/bge-m3/onnx
"""
import json
import pathlib
import sys

import numpy as np
import onnxruntime as ort
from tokenizers import Tokenizer

TEXTS = [
    "I went to the gym for 40 minutes",
    "Finished section 4 of the Rust course before work",
    "நான் உடற்பயிற்சி கூடத்துக்குச் சென்றேன்",
    "मैंने आज किताब के 20 पन्ने पढ़े",
    "今日は日本語の勉強をしました",
    "Played video games all evening 🎮",
    "SELECT count(*) FROM tasks WHERE status = 'open';",
    "  Meeting   with Priya\tabout the Q4  report  ",
]


def main():
    d = pathlib.Path(sys.argv[1])
    tok = Tokenizer.from_file(str(d / "tokenizer.json"))
    sess = ort.InferenceSession(str(d / "model.onnx"), providers=["CPUExecutionProvider"])
    out_names = [o.name for o in sess.get_outputs()]
    items = []
    for t in TEXTS:
        norm = " ".join(t.split())  # the embedder normalises whitespace the same way
        ids = tok.encode(norm).ids
        feeds = {"input_ids": np.array([ids], dtype=np.int64), "attention_mask": np.ones((1, len(ids)), dtype=np.int64)}
        res = dict(zip(out_names, sess.run(None, feeds)))
        v = res["sentence_embedding"][0] if "sentence_embedding" in res else res[out_names[0]][0, 0]
        v = v / np.linalg.norm(v)
        items.append({"text": t, "ids": ids, "vector": [round(float(x), 6) for x in v]})
    path = pathlib.Path(__file__).with_name("reference.json")
    path.write_text(json.dumps({"model": "bge-m3 onnx fp32, CPU", "items": items}, ensure_ascii=False) + "\n")
    print(f"wrote {path} ({len(items)} texts)")


if __name__ == "__main__":
    main()
