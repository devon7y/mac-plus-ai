#!/bin/bash
# Download FLUX.1-schnell (diffusers layout only) from the ungated Apache-2.0 mirror
# unsloth/FLUX.1-schnell, pinned to one commit, plus the OpenCLIP ViT-H-14 LAION-2B weights (MIT)
# used by filter.py. Verifies sha256 of every .safetensors against hashes agreed by independent
# mirrors and writes the licence and provenance files next to the model and the data.
# Usage (login node or CPU job; needs internet):  bash download_models.sh /scratch/devon7y/macplus_dream
set -uo pipefail
P=${1:?project dir}
REPO=unsloth/FLUX.1-schnell
REV=9df3faa7ae3b6ddf0b2b69bb78616372897cc65c          # HF API "sha" on 2026-10-16
M=$P/models/FLUX.1-schnell
CLIP_REPO=laion/CLIP-ViT-H-14-laion2B-s32B-b79K
CLIP_REV=1c2b8495b28150b8a4922ee1c8edee224c284c0c
C=$P/models/CLIP-ViT-H-14-laion2B-s32B-b79K
mkdir -p $M $C $P/data

FILES="model_index.json README.md NOTICE scheduler/scheduler_config.json
text_encoder/config.json text_encoder/model.safetensors
text_encoder_2/config.json text_encoder_2/model.safetensors.index.json
text_encoder_2/model-00001-of-00002.safetensors text_encoder_2/model-00002-of-00002.safetensors
tokenizer/merges.txt tokenizer/special_tokens_map.json tokenizer/tokenizer_config.json tokenizer/vocab.json
tokenizer_2/special_tokens_map.json tokenizer_2/spiece.model tokenizer_2/tokenizer.json tokenizer_2/tokenizer_config.json
transformer/config.json transformer/diffusion_pytorch_model.safetensors.index.json
transformer/diffusion_pytorch_model-00001-of-00003.safetensors
transformer/diffusion_pytorch_model-00002-of-00003.safetensors
transformer/diffusion_pytorch_model-00003-of-00003.safetensors
vae/config.json vae/diffusion_pytorch_model.safetensors"

get() {  # repo rev path dest
  mkdir -p "$(dirname "$4")"
  curl -sSL --fail --retry 5 --retry-delay 10 -C - -o "$4" "https://huggingface.co/$1/resolve/$2/$3" \
    && echo "[$(date +%T)] ok $3" || echo "[$(date +%T)] FAILED $3"
}
echo "[$(date)] downloading $REPO@$REV"
for f in $FILES; do
  if [ -s $M/$f ] && [[ $f != *.safetensors ]]; then continue; fi
  get $REPO $REV $f $M/$f &
  while [ $(jobs -r | wc -l) -ge 4 ]; do sleep 2; done
done
get $CLIP_REPO $CLIP_REV open_clip_config.json $C/open_clip_config.json &
get $CLIP_REPO $CLIP_REV open_clip_model.safetensors $C/open_clip_model.safetensors &
get $CLIP_REPO $CLIP_REV README.md $C/README.md &
wait
curl -sSL --fail -o $M/LICENSE-Apache-2.0.txt https://www.apache.org/licenses/LICENSE-2.0.txt

cat > $P/models/expected_sha256.txt <<'EOF'
893d67a23f4693ed42cdab4cbad7fe3e727cf59609c40da28a46b5470f9ed082  text_encoder/model.safetensors
ec87bffd1923e8b2774a6d240c922a41f6143081d52cf83b8fe39e9d838c893e  text_encoder_2/model-00001-of-00002.safetensors
a5640855b301fcdbceddfa90ae8066cd9414aff020552a201a255ecf2059da00  text_encoder_2/model-00002-of-00002.safetensors
9b633dbe87316385c5b1c262bd4b5a01e3d955170661d63dcec8a01e89c0d820  transformer/diffusion_pytorch_model-00001-of-00003.safetensors
58b4434078f0c2567ddc54e3b5cbf39626ab55fbd9d5c22956e183668f535dec  transformer/diffusion_pytorch_model-00002-of-00003.safetensors
e2cbc25471ed5186e69a9b51098300cb2f612556453e38a372c851a220ed238d  transformer/diffusion_pytorch_model-00003-of-00003.safetensors
f5b59a26851551b67ae1fe58d32e76486e1e812def4696a4bea97f16604d40a3  vae/diffusion_pytorch_model.safetensors
EOF
echo "0084e75319a50ad85ef45377bad5bc38f2f58824459eb690048d51c9f8863be5  open_clip_model.safetensors" > $P/models/expected_sha256_clip.txt
echo "[$(date)] sha256 check"
(cd $M && nice sha256sum $(awk '{print $2}' $P/models/expected_sha256.txt) | tee $M/sha256sum.txt && sha256sum -c $P/models/expected_sha256.txt)
(cd $C && nice sha256sum open_clip_model.safetensors | tee $C/sha256sum.txt && sha256sum -c $P/models/expected_sha256_clip.txt)

cp $M/LICENSE-Apache-2.0.txt $M/README.md $M/NOTICE $P/data/
cat > $P/data/PROVENANCE.txt <<EOF
Images in this folder were generated with FLUX.1-schnell (Black Forest Labs, Apache-2.0).
Source: https://huggingface.co/$REPO (ungated, byte-identical mirror of black-forest-labs/FLUX.1-schnell)
Commit: $REV
Downloaded: $(date -u +%Y-%m-%dT%H:%MZ) (diffusers layout only)
Licence: Apache License 2.0 (LICENSE-Apache-2.0.txt; model card README.md; NOTICE)
sha256 of the weights (all match the hashes published by independent mirrors):
$(cat $M/sha256sum.txt)

Filtering uses OpenCLIP ViT-H-14 trained on LAION-2B ($CLIP_REPO@$CLIP_REV, MIT licence;
open_clip code MIT). sha256: $(cat $C/sha256sum.txt)
EOF
du -sh $M $C
echo "[$(date)] done"
