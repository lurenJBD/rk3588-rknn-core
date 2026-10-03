# Ling-3.0-tiny W4A8 (RKNN) LLM Result

A recorded LLM workload on the reference platform. The RKNN model is a
third-party build by Sariel00:

- Project: <https://huggingface.co/Sariel00/Ling-3.0-tiny-RKNN>

| Item | Value |
|---|---|
| Model | Ling-3.0-tiny W4A8 |
| Context / IO | 8K context, 128 tokens in / 64 out |
| Time to first token | **829.875 ms** (median) |
| Decode | **11.455 tok/s** |
| Peak RSS | **5767 MiB** |
| Result | PASS (self-check passed; all three NPU cores generated IRQs) |

This test suite does not ship a runner for this workload. Build and run it from
the project above; the toolkit-demo scores are in [scores.md](scores.md).
