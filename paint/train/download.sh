#!/bin/bash
# Download all 345 Quick, Draw! sketch-rnn stroke-3 npz files (CC BY 4.0).
cd /scratch/devon7y/macplus_paint/data
mkdir -p npz
while IFS= read -r c; do
  [ -z "$c" ] && continue
  echo "$c"
done < categories.txt | xargs -P 8 -I{} bash -c '
  c="{}"; f="npz/$c.npz"; u="https://storage.googleapis.com/quickdraw_dataset/sketchrnn/${c// /%20}.npz"
  if [ ! -s "$f" ]; then curl -fsS --retry 5 -o "$f.part" "$u" && mv "$f.part" "$f" || echo "FAIL $c"; fi'
echo DONE $(ls npz/*.npz | wc -l)
