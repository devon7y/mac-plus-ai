#!/bin/bash
# Download FLUX.2 [klein] 4B (diffusers layout only; Apache-2.0, ungated official repo), pinned to
# one commit, verify sha256 of the weights, keep LICENSE.md and README.md next to them.
# Usage: bash download_klein.sh <project dir>
set -uo pipefail
P=${1:?project dir}
REPO=black-forest-labs/FLUX.2-klein-4B
REV=e7b7dc27f91deacad38e78976d1f2b499d76a294          # HF API "sha" on 2026-10-16
M=$P/models/FLUX.2-klein-4B
mkdir -p $M
FILES="LICENSE.md README.md model_index.json scheduler/scheduler_config.json
text_encoder/config.json text_encoder/generation_config.json text_encoder/model.safetensors.index.json
text_encoder/model-00001-of-00002.safetensors text_encoder/model-00002-of-00002.safetensors
tokenizer/added_tokens.json tokenizer/chat_template.jinja tokenizer/merges.txt tokenizer/special_tokens_map.json
tokenizer/tokenizer.json tokenizer/tokenizer_config.json tokenizer/vocab.json
transformer/config.json transformer/diffusion_pytorch_model.safetensors
vae/config.json vae/diffusion_pytorch_model.safetensors"
get() {
  mkdir -p "$(dirname "$M/$1")"
  curl -sSL --fail --retry 5 --retry-delay 10 -C - -o "$M/$1" "https://huggingface.co/$REPO/resolve/$REV/$1" \
    && echo "[$(date +%T)] ok $1" || echo "[$(date +%T)] FAILED $1"
}
echo "[$(date)] downloading $REPO@$REV"
for f in $FILES; do
  if [ -s $M/$f ] && [[ $f != *.safetensors ]]; then continue; fi
  get $f &
  while [ $(jobs -r | wc -l) -ge 4 ]; do sleep 2; done
done
wait
cat > $M/expected_sha256.txt <<'EOF'
9f29f9edcfdae452a653ffb51a534ca4decd389952c225724ff3b94042612a6e  transformer/diffusion_pytorch_model.safetensors
8c0506e7f4936fa7e26183a4fd8da4e2bdbc5990ba64ae441f965d51228f36ea  text_encoder/model-00001-of-00002.safetensors
82f2bd839378541b0557bfabaf37c7d3d637071fdcb73302dedd7cf61162ce07  text_encoder/model-00002-of-00002.safetensors
ca70d2202afe6415bdbcb8793ba8cd99fd159cfe6192381504d6c4d3036e0f04  vae/diffusion_pytorch_model.safetensors
EOF
echo "[$(date)] sha256 check"
(cd $M && nice sha256sum $(awk '{print $2}' expected_sha256.txt) | tee sha256sum.txt && sha256sum -c expected_sha256.txt)
cat > $M/PROVENANCE.txt <<EOF
FLUX.2 [klein] 4B, Black Forest Labs. Source: https://huggingface.co/$REPO (official, ungated)
Commit: $REV   Downloaded: $(date -u +%Y-%m-%dT%H:%MZ) (diffusers layout only)
Licence: see LICENSE.md (model card metadata: apache-2.0)
sha256:
$(cat $M/sha256sum.txt)
EOF
du -sh $M
echo "[$(date)] done"
