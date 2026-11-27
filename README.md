# Thermodynamic Objective for Differential Search

Source code for differential trail clustering experiments on SKINNY, Midori, PRESENT, GIFT, TWINE, and CRAFT. The repository contains 22 configurations.

## Quick start

Use Linux or WSL with a C++17 compiler. On Ubuntu/Debian:

```bash
sudo apt install g++ libgmp-dev libsqlite3-dev zlib1g-dev libzstd-dev
bash run.sh skinny64_10 runs/skinny64_10 --cores 60 --hours 1
bash run.sh gift128_21 runs/gift128_21 --cores 60 --hours 1
```

Run commands from the repository root. Required executables are built automatically. Results and `build.log` are written to the specified output directory. Use `--seed N` to set the random seed.

## Supported configurations

| Cipher | Variant names |
|---|---|
| SKINNY-64 | `skinny64_8`, `skinny64_9`, `skinny64_10`, `skinny64_11` |
| SKINNY-128 | `skinny128_14`, `skinny128_15`, `skinny128_16` |
| Midori64 | `midori64_8`, `midori64_9`, `midori64_10` |
| Midori128 | `midori128_9`, `midori128_10` |
| PRESENT | `present16`, `present17` |
| GIFT-64 | `gift64_13`, `gift64_14` |
| GIFT-128 | `gift128_20`, `gift128_21` |
| TWINE | `twine64_14`, `twine64_15` |
| CRAFT | `craft64_13`, `craft64_14` |

## Resuming

SKINNY, Midori, PRESENT, and GIFT resume automatically when rerun with the same output directory; `--hours` specifies the total elapsed-time budget.

TWINE and CRAFT require `--resume`; `--hours` specifies additional time:

```bash
bash run.sh twine64_14 runs/twine64_14 --cores 60 --hours 1 --resume
```

## Results

Runs save certified probability lower bounds and resumable state. A saved bound may represent a partial sum when the time budget expires. Fresh runs start without saved results or literature input/output differences.
