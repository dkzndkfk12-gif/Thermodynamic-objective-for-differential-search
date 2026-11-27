# Thermodynamic differential search

논문에 사용된 6종 암호의 22개 실험 소스입니다. 기존 결과, SQLite 장부, 실행 바이너리, 분석 자료는 포함하지 않습니다. 원본 폴더는 변경하지 않았습니다.

## 실행

Ubuntu/Linux 또는 WSL에서 Python 3, C++17 컴파일러, GMP, SQLite, zlib, Zstandard 개발 패키지가 필요합니다.

```bash
sudo apt install g++ python3 libgmp-dev libsqlite3-dev zlib1g-dev libzstd-dev
bash run.sh skinny64_10 runs/skinny64_10 --cores 100 --hours 1
bash run.sh gift128_21 runs/gift128_21 --cores 100 --hours 1
```

`--seed N`으로 난수 시드를 지정할 수 있습니다. 결과는 지정한 출력 폴더에 저장되며, 빌드 내역은 그 폴더의 `build.log`에서 확인합니다. 최초 실행 시 필요한 계산기를 빌드합니다. `Ctrl+C`로 중단할 수 있습니다.

SKINNY·Midori·PRESENT·GIFT는 같은 출력 폴더로 다시 실행하면 저장된 계산을 이어갑니다. 이때 `--hours`는 누적 실행 시간입니다. TWINE·CRAFT는 재개할 때 `--resume`을 붙이며, `--hours`는 재개 후 추가 시간입니다.

```bash
bash run.sh twine64_14 runs/twine64_14 --cores 60 --hours 1 --resume
```

## 포함한 실험

| 암호 | 실행 이름 | 코드 위치 |
|---|---|---|
| SKINNY-64 | skinny64_8, skinny64_9, skinny64_10, skinny64_11 | skinny_midori |
| SKINNY-128 | skinny128_14, skinny128_15, skinny128_16 | skinny_midori |
| Midori64 | midori64_8, midori64_9, midori64_10 | skinny_midori |
| Midori128 | midori128_9, midori128_10 | skinny_midori |
| PRESENT | present16, present17 | present |
| GIFT-64 | gift64_13, gift64_14 | gift/gift64 |
| GIFT-128 | gift128_20 | gift/gift128_20 |
| GIFT-128 | gift128_21 | gift/gift128_21, gift/shared |
| TWINE | twine64_14, twine64_15 | twine |
| CRAFT | craft64_13 | craft/craft13 |
| CRAFT | craft64_14 | craft/craft14 |

각 버전의 원래 위치는 `SOURCES.json`에 기록했습니다. PRESENT는 원고 결과를 얻은 Python 제어부와 C++ 계산기의 결합 버전입니다. 서로 다른 버전에서 얻은 GIFT-128 및 CRAFT 결과는 버전별 소스를 구분하여 보존했습니다.

## 정리한 사항

확률 임계값 F, adaptive threshold 판정, 완료 소문제 수 제한, 확률에 따른 저장 필터, 종료 후 최고 후보의 exact 강제 완주 기능을 제거했습니다. 일반 실행에서는 `--cores`, `--hours`, `--seed`만 지정합니다. 기존 실행에 사용한 replica, 공동 pattern 탐색, AIS, DP, 정확한 정수 합산과 암호별 기본 매개변수는 유지했습니다. exact 계산의 adaptive conditioning은 합산 알고리즘이므로 threshold 판정과 구분하여 유지했습니다.

시간 예산에 도달하면 진행 중 작업을 회수하고 확보한 부분합과 재개 상태를 저장합니다. 종료 후 별도의 최고 후보 완주 단계는 없습니다. 새 실행에는 기존 결과나 문헌의 입출력 차분을 주입하지 않습니다.

터미널에는 경과 시간, 현재 최고 인증 하한, 그 입력·출력 차분을 한 줄로 표시합니다. 초기 준비 중에는 `pending`으로 표시하며, 종료 또는 오류 시 상태를 덧붙입니다. CPU·RAM·pattern·대기열 통계는 터미널에 출력하지 않습니다.

```text
elapsed=12.3 min | best=2^-57.631216257 | 0000000000000004 -> E0800080E000E080
```

코드의 설명 주석과 docstring을 제거했습니다. 실행에 필요한 shebang과 C/C++ 전처리 지시문은 유지했고, 외부 헤더의 저작권·라이선스 고지는 `THIRD_PARTY_NOTICES.txt`에 분리했습니다.
