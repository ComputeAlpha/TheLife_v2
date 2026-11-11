"""Measures this process's GPU memory for bge-m3 under different settings (one config per process)."""
import os, subprocess, sys, time
import numpy as np, onnxruntime as ort
model, strategy, max_batch = sys.argv[1], sys.argv[2], int(sys.argv[3])
opts = {"device_id": 0, "arena_extend_strategy": strategy}
s = ort.InferenceSession(model, providers=[("CUDAExecutionProvider", opts)])
assert "CUDAExecutionProvider" in s.get_providers()
ids = np.ones((max_batch, 512), dtype=np.int64) * 100; ids[:, 0] = 0; ids[:, -1] = 2
mask = np.ones_like(ids)
t0 = time.monotonic()
for bs in (1, max_batch):
    out = s.run(["sentence_embedding"], {"input_ids": ids[:bs], "attention_mask": mask[:bs]})[0]
dt = time.monotonic() - t0
pid = str(os.getpid())
used = [l.split(",")[1].strip() for l in subprocess.run(["nvidia-smi", "--query-compute-apps=pid,used_memory",
        "--format=csv,noheader,nounits"], capture_output=True, text=True).stdout.splitlines() if l.split(",")[0].strip() == pid]
print(f"{os.path.basename(os.path.dirname(model)) or model:10} strategy={strategy:18} max_batch={max_batch:2} seq=512 "
      f"gpu_mib={used[0] if used else '?'} out_dtype={out.dtype}")
