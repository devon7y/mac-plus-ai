#!/usr/bin/env python3
"""Build Pixel's allowed-word list (child-level vocabulary).

Source: the 2,000 most frequent lower-cased word forms of TinyStoriesV2-GPT4-valid.txt
(TinyStories, CDLA-Sharing-1.0; only word frequencies are used), plus hand-curated chat words,
persona words (Pixel, floppy, Chicago, magnets ...), numbers, colours, days, months, animals,
foods and ~150 common first names.

Usage: python3 build_vocab.py <TinyStoriesV2-GPT4-valid.txt> <out vocab.txt> [top_n]
Output: one lower-case word per line, sorted. Words may contain an apostrophe (don't, pixel's).
"""
import collections
import re
import sys

CHAT = """
hi hello hey hiya bye goodbye goodnight morning afternoon evening night yes no yeah yep nope ok okay
please thanks thank sorry welcome oops wow yay hooray hmm oh ah aha uh um huh ha haha hehe lol
cool awesome great nice fun funny silly sure maybe really very so too also just still again
favorite favourite fave like likes liked love loves loved enjoy enjoys want wants wanted need
how what why when where who which whose whom am are is was were be been being do does did done
can could will would shall should may might must have has had having i'm i'll i've i'd you're
you'll you've you'd he's she's it's we're we've they're they'll that's what's where's who's
how's there's here's let's don't doesn't didn't can't couldn't won't wouldn't isn't aren't
wasn't weren't haven't hasn't shouldn't i you he she it we they me him her us them my your his its
our their mine yours ours theirs myself yourself friend friends buddy pal chat chatting talk talking
talked question questions answer answers joke jokes riddle riddles guess guessing game games play
playing played fun bored boring tired sleepy hungry thirsty sad happy glad mad angry scared afraid
worried nervous excited lonely shy proud calm cozy grumpy upset feel feels feeling feelings okay
alright fine today tomorrow yesterday now later soon never always sometimes often week weekend
weekends day days hour hours minute minutes second seconds time times year years month months
age old new young little big small tiny huge
""".split()

PERSONA = """
pixel pixel's macintosh mac mac's computer computers screen screens keyboard keyboards mouse mice
floppy floppies disk disks drive desk desks chicago font fonts letter letters beep beeps beeped
beeping boop click clicks clicking magnet magnets type types typing typed button buttons cursor
icon icons window windows menu picture pictures pixel pixels black white gray grey dot dots
plug cord power switch electric bits byte bytes memory remember remembers forget forgets forgot
think thinks thought calendar clock year 1986 nineteen eighty six robot
""".split()

NUMBERS = """
zero one two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen
sixteen seventeen eighteen nineteen twenty thirty forty fifty sixty seventy eighty ninety hundred
thousand first second third fourth fifth sixth seventh eighth ninth tenth half count counting
counted number numbers plus minus add equals more less most least many much few lots
""".split()

COLOURS_TIME = """
red orange yellow green blue purple pink brown black white gray grey gold silver colors colours
color colour rainbow monday tuesday wednesday thursday friday saturday sunday january february
march april may june july august september october november december spring summer fall autumn
winter season seasons
""".split()

THINGS = """
cat cats kitten kittens dog dogs puppy puppies fish bird birds frog frogs duck ducks horse horses
cow cows pig pigs sheep goat rabbit rabbits bunny bunnies bear bears lion lions tiger tigers
elephant elephants monkey monkeys owl owls bee bees butterfly butterflies snail snails turtle
turtles giraffe giraffes zebra penguin penguins whale whales dolphin dolphins shark sharks
dinosaur dinosaurs bug bugs ant ants spider spiders worm worms hamster mouse chicken chickens
fox foxes wolf deer squirrel squirrels snake snakes crab octopus parrot puppy legs leg tail tails
wings wing fur paws apple apples banana bananas pizza cake cakes cookie cookies ice cream soup
bread toast carrot carrots milk egg eggs pancake pancakes sandwich cheese candy chocolate juice
water tea grapes orange oranges strawberry strawberries pie popcorn pasta noodles rice salad
breakfast lunch dinner snack snacks yummy yum tasty sweet rain rainy snow snowy sun sunny wind
windy cloud clouds cloudy storm stormy weather hot cold warm cool fog foggy puddle puddles
square circle triangle star stars heart shape shapes moon sky sea ocean beach tree trees flower
flowers park garden school teacher class homework book books story stories song songs music sing
singing dance dancing draw drawing paint painting crayon crayons toy toys ball blocks puzzle
puzzles bike kite doll teddy robot birthday party present presents gift balloon balloons bedtime
bed dream dreams pajamas blanket pillow home house room kitchen family mom mommy dad daddy
brother sister grandma grandpa baby friends hide seek tag spy rock paper scissors win won lose
lost turn turns
""".split()

NAMES = """
sam max leo mia lily emma noah liam ava zoe ben tom tim anna amy jack jake lucy sara sarah
kate katie maya ella ruby rosie rose ivy grace eli finn owen luke mark john james will ollie
oliver jacob ethan alex sophie sofia chloe olivia harry henry charlie archie oscar isla poppy
daisy molly holly jess joe jen dan dave mike nick nora nina lola luna milo theo jay kai ray rex
ali omar sami aisha zara priya ana luis jose carlos maria rosa yuki kenji mei lin wei tara dev
devon sid raj ravi asha hana lea leah eva evie abby beth bella bob bobby billy bill fred frank
george gus hank ian ivan jade jane jill josh kim kevin lisa lucas mason matt mila nate nell
pam pat paul pete peter rob ryan sally sean tess tina toby tony una vera wes zack zoey
""".split()

# Words to drop from the TinyStories list (names of story places, rare/unsuitable for Pixel).
DROP = set("""
god gun guns kill killed die died dead death blood war police prison jail
""".split())


def main():
    src, out = sys.argv[1], sys.argv[2]
    top_n = int(sys.argv[3]) if len(sys.argv) > 3 else 2000
    text = open(src, encoding="utf-8", errors="ignore").read().replace("’", "'")
    words = re.findall(r"[A-Za-z]+(?:'[a-z]+)?", text)
    counts = collections.Counter(w.lower() for w in words)
    vocab = {w for w, _ in counts.most_common(top_n)}
    for extra in (CHAT, PERSONA, NUMBERS, COLOURS_TIME, THINGS, NAMES):
        vocab.update(w.lower() for w in extra)
    vocab -= DROP
    vocab = sorted(vocab)
    with open(out, "w") as f:
        f.write("\n".join(vocab) + "\n")
    print(f"{len(vocab)} words -> {out}")


if __name__ == "__main__":
    main()
