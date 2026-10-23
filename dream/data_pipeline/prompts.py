"""Prompt and seed construction for Dream's FLUX.1-schnell training data.

Every image is identified by (subject, i). The prompt is drawn from a random.Random seeded with
the string "subject|i" (deterministic across machines and Python versions), and the FLUX noise
seed is category_index * 100000 + i, where category_index is the subject's line in the stable
345-name Quick, Draw! list (paint/app/data/categories.txt, copied here as categories.txt).
"""
import json
import os
import random

HERE = os.path.dirname(os.path.abspath(__file__))


def read_list(path):
    with open(path) as f:
        return [l.split('\t')[0].strip() for l in f if l.strip()]


CATEGORIES = read_list(os.path.join(HERE, 'categories.txt'))
SUBJECTS = read_list(os.path.join(HERE, 'subjects.txt'))

# The noun phrase used in prompts and CLIP texts when the Quick, Draw! name is ambiguous or awkward.
PHRASE = {
    'bat': 'bat animal with spread wings', 'mouse': 'mouse animal', 'fan': 'electric fan',
    'star': 'five-pointed star', 'drill': 'power drill', 'key': 'door key',
    'lightning': 'lightning bolt', 'moon': 'crescent moon', 'diamond': 'diamond gemstone',
    'bowtie': 'bow tie', 'flip flops': 'pair of flip flops', 'rollerskates': 'roller skate',
    'pants': 'pair of trousers', 'shorts': 'pair of shorts', 'eyeglasses': 'pair of eyeglasses',
    'binoculars': 'pair of binoculars', 'scissors': 'pair of scissors', 'pliers': 'pair of pliers',
    'headphones': 'pair of headphones', 'grapes': 'bunch of grapes', 'peas': 'pea pod',
    'drums': 'drum', 'matches': 'matchstick', 'teddy-bear': 'teddy bear',
    'The Eiffel Tower': 'Eiffel Tower', 'see saw': 'seesaw', 'bread': 'loaf of bread',
    'ice cream': 'ice cream cone', 'firetruck': 'fire truck', 'motorbike': 'motorcycle',
    'flying saucer': 'flying saucer', 'house plant': 'potted house plant',
    'tennis racquet': 'tennis racket', 'computer': 'desktop computer', 'television': 'television set',
    'hand': 'human hand', 'foot': 'human foot', 'eye': 'human eye', 'paint can': 'can of paint',
    'cell phone': 'mobile phone', 'wheel': 'wagon wheel', 'steak': 'T-bone steak',
    'smiley face': 'smiley face', 'sun': 'sun with rays',
}
THE = {'The Eiffel Tower'}                 # proper nouns: "the Eiffel Tower", no modifiers
AN_EXCEPT = ('uni', 'one', 'eu', 'use')   # vowel-initial words that take "a"
AN_EXTRA = ('hour',)                      # consonant-initial words that take "an"

ANIMALS = set('''ant bat bear bee bird butterfly camel cat cow crab crocodile dog dolphin dragon duck
elephant fish flamingo frog giraffe hedgehog horse kangaroo lion lobster monkey mosquito mouse octopus
owl panda parrot penguin pig rabbit raccoon rhinoceros scorpion sea turtle shark sheep snail snake
spider squirrel swan tiger whale zebra'''.replace('sea turtle', 'sea_turtle').split())
ANIMALS = {a.replace('_', ' ') for a in ANIMALS}
VEHICLES = set('''airplane ambulance bicycle bulldozer bus canoe car cruise ship firetruck helicopter
motorbike pickup truck police car sailboat speedboat submarine tractor train truck van'''
               .replace('cruise ship', 'cruise_ship').replace('pickup truck', 'pickup_truck')
               .replace('police car', 'police_car').split())
VEHICLES = {v.replace('_', ' ') for v in VEHICLES}

MODS_ANIMAL = ['', '', 'cute ', 'simple ', 'cartoon ', 'friendly ', 'happy ']
MODS_OBJECT = ['', '', 'simple ', 'old-fashioned ', 'cartoon ', 'cute ', 'classic ']

TEMPLATES = [
    'simple black and white clip art of {a} {m}{s}{v}, bold black outlines, centered, plain white background, no text, no border',
    '{A} {m}{s}{v}, black and white line art clip art, thick outlines, solid black fills, isolated on a white background, centered',
    'vintage 1980s computer clip art of {a} {m}{s}{v}, black ink on white paper, simple shapes, bold outlines, white background',
    '{a} {m}{s} icon{v}, black and white, bold outline, simple flat clip art, centered on a plain white background',
    'black and white woodcut-style clip art of {a} {m}{s}{v}, bold lines, simple, centered, white background, no text',
    'a single {m}{s}{v}, monochrome clip art illustration, bold black ink outlines, solid shapes, centered on pure white, no text, no frame',
]


# Full captions for FLUX 3 Image Klein, which expects upsampler-style prose (medium and style first,
# layout in words, negations stated as what is present). {tag} is '' or ' <subject_1>' (layout box).
CAPTIONS = [
    'Very simple black and white clip art icon of {a} {m}{s}{tag}{v}, in the style of 1980s computer clip '
    'art: a few thick, bold black outlines and large flat solid black shapes, with minimal detail. The {s} '
    'sits alone in the center of a square white canvas, shown whole, filling about two thirds of the frame, '
    'with an empty white margin on every side. Every shape is pure black or pure white, flat and crisp, with '
    'no shading, hatching, texture, fine lines or gray tones. The background is plain, empty white. The '
    'picture contains only the single {s}, with no ground line, cast shadow, lettering, border or frame.',
    'Monochrome vector icon of {a} {m}{s}{tag}{v}, drawn with a few thick uniform black outlines and solid '
    'black filled areas, reduced to its simplest recognizable shape. The {s} is centered on a pure white '
    'square background, drawn complete and large, occupying the middle two thirds of the canvas with even '
    'white space around it. Flat graphic style, high contrast, only black ink on white, with no gradients, '
    'gray, shading, hatching or texture, and very little interior detail. Nothing else appears in the image: '
    'no text, letters, numbers, border, frame, shadow or scenery.',
    'Bold, simple black-and-white clip art drawing of {a} {m}{s}{tag}{v}, like a chunky early 1980s computer '
    'clip art image: thick black contour lines, large solid black shapes and clean white areas, with only the '
    'few details needed to recognize it. The single {s} stands in the center of the frame with its whole '
    'silhouette visible and a generous blank white margin around it. Pure black ink on a plain white '
    'background, flat and graphic, with no gray tones, gradients, hatching, texture or fine lines, and no '
    'text, caption, border, ground or shadow anywhere.',
    'Simple black ink cartoon of {a} {m}{s}{tag}{v} in a bold, minimal clip art style: smooth, thick black '
    'outlines of even width, some areas filled solid black and the rest left white, with very little detail. '
    'The {s} is drawn large and complete, centered on a plain white square background, filling roughly two '
    'thirds of the frame with an even white margin all around. High-contrast black and white only, flat, '
    'without shading, hatching, texture or gray. The image shows only the one {s} on white, with no text, '
    'border, frame, ground or background scenery.',
]
# Caption modifiers and pose/view phrases (M2): rotated per image so iconic subjects don't converge
# on one pose; only phrases that make sense for the subject's class are used. Style text is unchanged.
CAP_MODS = ['', '', '', '', 'simple ', 'simple ', 'cute ', 'cute ', 'cartoon ', 'cartoon ', 'detailed ', '']  # detailed 1/12: thin lines go grey
CHARACTERS = {'angel', 'mermaid', 'snowman', 'teddy-bear'}
FACING = ANIMALS | VEHICLES | CHARACTERS | {'skateboard', 'rollerskates', 'cannon'}
FACING_VIEWS = ['', ' in side view', ' in front view', ' in three-quarter view', ' facing left', ' facing right']
ORIENT = set('''axe baseball bat broom carrot clarinet crayon dumbbell feather flashlight fork hammer key knife
ladder lipstick matches microphone paintbrush pencil rake saw screwdriver shovel spoon sword syringe
tennis racquet toothbrush trombone trumpet saxophone violin cello asparagus banana popsicle lollipop drill
paper clip pliers scissors megaphone'''.replace('baseball bat', 'baseball_bat').replace('tennis racquet', 'tennis_racquet')
             .replace('paper clip', 'paper_clip').split())
ORIENT = {o.replace('_', ' ') for o in ORIENT}
ORIENT_VIEWS = ['', ' standing upright', ' tilted diagonally', ' lying horizontally']
NOVIEW = set('''baseball basketball cookie donut pizza sun moon star snowflake cloud lightning rainbow envelope
bowtie moustache necklace leaf flower wheel clock wristwatch eye hand foot tooth brain skull crown bandage
boomerang peanut pear strawberry watermelon pineapple grapes onion broccoli peas mushroom cactus tree
mountain campfire hourglass chandelier sock pants shorts sweater jacket steak diamond'''
             .replace('soccer ball', 'soccer_ball').split()) | {'soccer ball', 'smiley face', 'palm tree',
                                                               't-shirt', 'light bulb', 'house plant',
                                                               'ceiling fan', 'The Eiffel Tower'}
OBJECT_VIEWS = ['', ' in front view', ' in side view', ' in three-quarter view']
SPECIAL = {   # subjects that converged on one pose in the M0 pilot
    'apple': [' with a leaf on its stem', ' with a bare stem and no leaf', ' with two leaves'],  # no 'bite': logo look-alikes
    'umbrella': [' fully open, drawn as an outline with white panels', ' fully open with alternating black and '
                 'white panels', ' fully open and solid black', ' closed and furled, standing upright',
                 ' open and tilted to one side'],
    'guitar': [' standing upright', ' tilted diagonally', ' lying horizontally', ' standing upright, seen from the side'],
    'car': [' in side view', ' in side view, facing left', ' in side view, facing right', ' in front view',
            ' in three-quarter view'],
    'airplane': [' in side view', ' seen from above', ' in three-quarter view', ' flying diagonally upward',
                 ' facing left', ' facing right'],
}


def views_for(subject):
    if subject in SPECIAL:
        return SPECIAL[subject]
    if subject in FACING:
        return FACING_VIEWS
    if subject in ORIENT:
        return ORIENT_VIEWS
    if subject in NOVIEW:
        return ['']
    return OBJECT_VIEWS


BOX = [160, 160, 840, 840]   # [top, left, bottom, right] on Klein's 0-1000 layout grid


def phrase(subject):
    return PHRASE.get(subject, subject)


def article(words):
    w = words.lower()
    if w.startswith(AN_EXTRA):
        return 'an'
    if w[0] in 'aeiou' and not w.startswith(AN_EXCEPT):
        return 'an'
    return 'a'


def category_index(subject):
    return CATEGORIES.index(subject)


def seed_for(subject, i):
    return category_index(subject) * 100000 + i


def make_prompt(subject, i, template=None, style='short'):
    """Deterministic prompt for image i of a subject; returns (prompt, template_id, modifier, view).
    style: 'short' (one-line prompt, FLUX.1/FLUX.2 and the request given to Klein's upsampler),
    'caption' (hand-written full caption for Klein), 'caption_bbox' (caption plus a centred layout box).
    With template=k, always use template k with no modifier and no view (model comparisons)."""
    if style != 'short':
        return make_caption(subject, i, template, bbox=(style == 'caption_bbox'))
    r = random.Random(f'{subject}|{i}')
    t = r.randrange(len(TEMPLATES))
    s = phrase(subject)
    if subject in THE:
        m, art = '', 'the'
    else:
        m = r.choice(MODS_ANIMAL if subject in ANIMALS else MODS_OBJECT)
        art = None
    v = ''
    if subject in ANIMALS or subject in VEHICLES:
        v = r.choice(['', '', ', side view'])
    if template is not None:
        t, m, v = template, '', ''
    tpl = TEMPLATES[t]
    if art is None:
        art = article((m + s).strip())
    text = tpl.format(a=art, A=art.capitalize(), m=m, s=s, v=v)
    if text.startswith('a single the '):
        text = text.replace('a single the ', 'the ', 1)
    return text, t, m.strip(), v.strip(', ')


def make_caption(subject, i, template=None, bbox=False):
    r = random.Random(f'caption|{subject}|{i}')
    t = r.randrange(len(CAPTIONS)) if template is None else template
    s = phrase(subject)
    m = '' if subject in THE or template is not None else r.choice(CAP_MODS)
    v = '' if template is not None else r.choice(views_for(subject))
    art = 'the' if subject in THE else article((m + s).strip())
    text = CAPTIONS[t].format(a=art, A=art.capitalize(), m=m, s=s, v=v, tag=' <subject_1>' if bbox else '')
    if bbox:
        desc = f'{art.capitalize()} {m}{s} drawn in bold black outlines with solid black fills, whole and centered.'
        text += ' ' + json.dumps([{'id': 'subject_1', 'bbox': BOX, 'desc': desc}])
    return text, t, m.strip(), v.strip()


def slug(subject):
    return subject.lower().replace(' ', '_').replace('-', '_')


if __name__ == '__main__':
    import sys
    subs = sys.argv[1:] or ['apple', 'umbrella', 'hourglass', 'The Eiffel Tower', 'cat', 'scissors']
    for s in subs:
        for i in range(4):
            print(seed_for(s, i), make_prompt(s, i)[0])
