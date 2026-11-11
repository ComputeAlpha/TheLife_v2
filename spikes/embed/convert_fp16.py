#!/usr/bin/env python3
"""One-time conversion: bge-m3 ONNX fp32 → attention-fused fp16 (ORT transformers optimizer).

Result (spike 2026-11-12): cosine ≥ 0.99999 vs fp32, identical top-5 neighbours, 2–2.6x faster,
~1.5–2.0 GB GPU instead of ~2.8–3.8 GB. The naive onnxconverter_common conversion produces an invalid graph
(mixed float16/float in the attention-mask Sub node); use this script instead.

Usage: .venv/bin/python convert_fp16.py models/bge-m3/onnx/model.onnx models/bge-m3-fp16/model.onnx
"""
import pathlib
import sys

from onnxruntime.transformers.optimizer import optimize_model

src, dst = sys.argv[1], sys.argv[2]
pathlib.Path(dst).parent.mkdir(parents=True, exist_ok=True)
# opt_level=0: Python-side fusions only (no ORT pre-optimization pass, which needs torch).
m = optimize_model(src, model_type="bert", num_heads=16, hidden_size=1024, opt_level=0)
print("fused:", {k: v for k, v in m.get_fused_operator_statistics().items() if v})
m.convert_float_to_float16(keep_io_types=True)  # inputs int64, outputs float32 stay unchanged
m.save_model_to_file(dst, use_external_data_format=True)
print("saved", dst)
