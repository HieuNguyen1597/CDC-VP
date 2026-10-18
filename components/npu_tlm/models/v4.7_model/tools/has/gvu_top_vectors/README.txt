# Stage-by-stage vectors for the cycle-level vector-unit model

Reference values of SOFTMAX and LAYERNORM on data of ViT-B/16, written by `tools/has/tb_gvu_vec_export.cpp` from the
functional model (`has/gvu_softmax.h`, `has/gvu_layernorm.h`) in the default configuration of the instruction path.
The functional model is bit-exact against the reference images of the network, so a cycle-level model that reproduces
these files stage by stage computes what the network needs.

| File | Content |
|---|---|
| `softmax_vit_b0_h0.txt`, `softmax_vit_b6_h3.txt`, `softmax_vit_b11_h7.txt` | 32 rows of 197 scores of one attention head of block 0, 6 and 11 |
| `layernorm_vit_b0_ln1.txt`, `layernorm_vit_b6_ln1.txt`, `layernorm_vit_b11_ln1.txt` | 32 rows of 768 channels of the first LayerNorm of block 0, 6 and 11 |

32 rows are one group of lanes. The record layout (tags `CFG`, `EXP`, `RECIP`, `ROW`, `X`, `M`, `E`, `SIG`, `R`, `T`, `A`
for softmax; `CFG`, `GAMMA`, `BETA`, `M0MUL`, `TSMUL`, `RSQRT`, `ROW`, `X`, `S`, `D`, `V`, `Q`, `N`, `Y` for LayerNorm)
is described at the top of `tools/has/tb_gvu_vec_export.cpp`. Every line is a tag followed by decimal integers.

Suggested use: load `X` of the 32 rows and the tables into the Scratchpad of the model, run the pipeline, and compare
what each stage writes with the line of that stage (`M` after stage 1 of softmax, `E` and `SIG` after stage 2, `A` after
stage 3; `S` after stage 1 of LayerNorm, `V` after stage 2, `Q` after stages 3 and 4, `N`, `Y` after stages 4 and 5).

Points to keep in mind when the cycle-level model is compared:

- softmax requant parameters of these vectors: scale 2^30, shift 37, zero point -128; the shift `sh` of stage 3 differs
  from row to row because the exponent `E` of the reciprocal lookup does;
- the tables are the 256-entry reciprocal image and the 512-entry inverse-square-root image; the exponential table is
  the one of the head;
- LayerNorm keeps `S`, `D` and `V` at full precision here; `V` needs up to 38 bits on these rows.

To write the complete heads and cases (197 and 64 rows), or other row counts:

```
g++ -std=c++17 -O2 -I. tools/has/tb_gvu_vec_export.cpp -o tb_gvu_vec_export
./tb_gvu_vec_export <unit-test vector dir> <table dir> <out dir> 0
```

The vector directory is `has/vectors/gvu` of the data package; the table directory is written by
`tools/has/export_gvu_attn.py`, `export_gvu_ln.py` and `export_gvu_tables.py`.
