#!/bin/bash
# Auxiliary models and code for FLUX 3 Image Klein generation (all ungated, Apache-2.0), pinned:
#  - black-forest-labs/flux-3-image-klein-pup (prompt upsampler, Qwen3.5 9B)
#  - Falconsai/nsfw_image_detection (content filter, as loaded by BFL's reference CLI)
#  - github.com/black-forest-labs/flux-3-image-klein reference inference code (src/flux3 only)
# Usage: bash download_aux.sh <project dir>
set -uo pipefail
P=${1:?project dir}
PUP=black-forest-labs/flux-3-image-klein-pup;  PUP_REV=3f3edb52b2c55de0d49feab28598b68f7f8e4830
NSFW=Falconsai/nsfw_image_detection;           NSFW_REV=96cb0d0342c7afb80cab76ecc58b265fa44da256
REF_REV=e53f4ea697fe45db8f38fca91a90f5623f7f8f37
get() {  # repo rev file destdir
  mkdir -p "$(dirname "$4/$3")"
  curl -sSL --fail --retry 5 --retry-delay 10 -C - -o "$4/$3" "https://huggingface.co/$1/resolve/$2/$3" \
    && echo "[$(date +%T)] ok $1/$3" || echo "[$(date +%T)] FAILED $1/$3"
}
M=$P/models/flux-3-image-klein-pup
for f in README.md chat_template.jinja config.json generation_config.json preprocessor_config.json \
         processor_config.json system_prompts.json tokenizer.json tokenizer_config.json \
         video_preprocessor_config.json model.safetensors; do get $PUP $PUP_REV $f $M & done
N=$P/models/nsfw-classifier
for f in README.md config.json labels.json preprocessor_config.json model.safetensors; do get $NSFW $NSFW_REV $f $N & done
wait
cat > $M/expected_sha256.txt <<'EOF'
f5062a1fba3e63afe5c88349a311903fec06c959b362f1247127e63d5c860ed4  model.safetensors
06b9509352d2af50381ab2247e083b80d32d5c0aba91c272ca9ff729b6a0e523  tokenizer.json
EOF
echo "97b2ce64ec146884b37f98ee7944ca4891aa72f6827dc0cb10684a1cbecd5830  model.safetensors" > $N/expected_sha256.txt
(cd $M && sha256sum -c expected_sha256.txt)
(cd $N && sha256sum -c expected_sha256.txt)
printf 'Source: https://huggingface.co/%s commit %s (Apache-2.0)\n' $PUP $PUP_REV > $M/PROVENANCE.txt
printf 'Source: https://huggingface.co/%s commit %s (Apache-2.0)\n' $NSFW $NSFW_REV > $N/PROVENANCE.txt

R=$P/flux3_ref; mkdir -p $R
curl -sSL --fail -o $R/ref.tar.gz https://codeload.github.com/black-forest-labs/flux-3-image-klein/tar.gz/$REF_REV
tar -xzf $R/ref.tar.gz -C $R --strip-components=1 --wildcards '*/src/flux3/*' '*/LICENSE' '*/README.md' '*/pyproject.toml'
rm -f $R/ref.tar.gz
echo "github.com/black-forest-labs/flux-3-image-klein commit $REF_REV (Apache-2.0), src/flux3 only" > $R/PROVENANCE.txt
ls $R/src/flux3
du -sh $M $N
echo "[$(date)] done"
