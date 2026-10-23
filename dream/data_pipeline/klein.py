"""Batched text-to-image with FLUX 3 Image Klein (distilled flow-map checkpoint: 16 steps, no guidance).

Built on BFL's reference inference code (github.com/black-forest-labs/flux-3-image-klein, Apache-2.0;
put its src/ on PYTHONPATH). For one sample it computes exactly what flux3.pipeline.Pipeline.generate
does for text-to-image (same seeded CPU noise, same schedule, same decode); several samples are packed
into one DiT call (the reference DiT supports multi-sample packs; samples never attend to each other).
The only change for batching: the flow-map target time is given once per sample.
"""
import torch
from torch.nn.attention import SDPBackend, sdpa_kernel

import flux3.dit as fdit
from flux3.autoencoder import AutoEncoder, latent_ids, latent_shape
from flux3.checkpoint import is_flow_map, load_checkpoint
from flux3.dit import DiT, IN_CHANNELS
from flux3.pipeline import pack, scaled_ids
from flux3.sampling import timesteps
from flux3.text_encoder import TextEncoder


def _check_cudnn_attention():
    """The reference forces cuDNN attention; fall back to flash/efficient kernels if this torch lacks it."""
    q = torch.randn(1, 2, 16, 64, device='cuda', dtype=torch.bfloat16)
    try:
        with sdpa_kernel(SDPBackend.CUDNN_ATTENTION):
            torch.nn.functional.scaled_dot_product_attention(q, q, q)
        return 'cudnn'
    except RuntimeError:
        def packed_attention(q, k, v, bounds):
            with sdpa_kernel([SDPBackend.FLASH_ATTENTION, SDPBackend.EFFICIENT_ATTENTION, SDPBackend.MATH]):
                return _packed(q, k, v, bounds)
        fdit.packed_attention = packed_attention     # Block.forward looks the name up at call time
        return 'flash/efficient'


def _packed(q, k, v, bounds):
    _, heads, length, dim = q.shape
    out = torch.zeros(length, heads * dim, device=q.device, dtype=q.dtype)
    for start, end in bounds:
        if start != end:
            chunk = torch.nn.functional.scaled_dot_product_attention(q[:, :, start:end], k[:, :, start:end], v[:, :, start:end])
            out[start:end] = chunk[0].transpose(0, 1).reshape(end - start, heads * dim)
    return out


@torch.inference_mode()
@torch.autocast('cuda', dtype=torch.bfloat16)
def denoise_flow_map(dit, packed, schedule):
    n = packed.num_samples
    for step in range(len(schedule) - 1):
        t, dt = schedule[step].item(), (schedule[step + 1] - schedule[step]).item()
        packed.timesteps['image'].fill_(t)
        target = schedule[step + 1].reshape(1).repeat(n).to(packed.data['image'].device)
        packed.data['image'] = packed.data['image'] + dt * dit(packed, timesteps_ref=target)
    return packed.data['image']


class Klein:
    name = 'flux-3-image-klein (distilled, reference code)'

    def __init__(self, root, device='cuda'):
        if not is_flow_map(root):
            raise SystemExit(f'{root}: expected the distilled flow-map checkpoint')
        self.attention = _check_cudnn_attention()
        with torch.device('meta'):
            dit = DiT(True)
        self.dit = load_checkpoint(dit, root, device, torch.bfloat16)
        self.text_encoder = TextEncoder(f'{root}/text_encoder', device)
        self.autoencoder = load_checkpoint(AutoEncoder(), f'{root}/ae.safetensors', device, torch.float32)
        self.device = device

    @torch.inference_mode()
    def __call__(self, captions, seeds, size=512, steps=16, alpha=6.93):
        """-> uint8 array (B, size, size, 3)."""
        h, w = latent_shape(size, size)
        ids = latent_ids(h, w, self.device)
        samples = []
        for cap, seed in zip(captions, seeds):
            noise = torch.randn((h * w, IN_CHANNELS['image']), generator=torch.Generator().manual_seed(int(seed)))
            noise = noise.to(torch.bfloat16).float().to(self.device)   # as the reference: bf16 values, fp32 state
            samples.append([self.text_encoder.encode(cap), None, (noise, scaled_ids(ids))])
        packed = pack(samples)
        tokens = denoise_flow_map(self.dit, packed, timesteps(steps, alpha))
        out = []
        for a, b in packed.mode_indices['image']:
            img = self.autoencoder.decode(tokens[a:b], ids)
            out.append(((img.float().clamp(-1, 1) + 1) * 127.5).to(torch.uint8).permute(1, 2, 0))
        return torch.stack(out).cpu().numpy()
