from typing import *
import re
import random

d = {}
chunks = []
endingChars = ['.', '?', '!', '\n']

def createSelfDictionary():
    for ch in chunks:
        words = ch.split()
        words.pop()
        del words[0]

        for w in words:
            d[w] = 1

def createDictionary(langFile: str):
    global d
    f = open(langFile)
    for w in f:
        w = w.rstrip('\n')
        w = w.rstrip(' ')
        d[w] = 1

def checkValidityChunk(chunk: str) -> bool:
    words = chunk.split()
    if len(words) > 2:
        words = words[1:-1]

    return all(w.lower() in d for w in words if w.isalpha())

def checkValidityText(current_text: str) -> bool:
    #start and end check
    if not current_text[0].isupper() or not current_text[-1] in endingChars:
       return False

    #all chunks test
    for chunk in chunks:
        if chunk not in current_text:
            return False
    
    #sentence check
    dot_positions = [i for i, ch in enumerate(current_text) if ch == '.']
    for i in dot_positions[:-1]:
        j = i + 1

        while j < len(current_text) and current_text[j].isspace():
            j += 1

        if j == len(current_text):
            return True
    
    nxt = current_text[j]
    if nxt != '.' or not nxt.isdigit() or not nxt.isupper():
        return False
    
    words = re.findall(r"[A-Za-z]+", current_text)
    return all(w.lower() in d for w in words if w.isalpha()) #words valid check
                
def backtrack(current_text: str, remaining: List[str], file, results: List[str]):
        if not remaining:
            if checkValidityText:
                print("found!\a")
                results.append(current_text)
                file.write(f"{current_text}\n")
            return
        
        for i, ch in enumerate(remaining):
            candidate = current_text + ch
            if checkValidityChunk(candidate):
                #file.write(f"joined chunks to: {candidate}\n")
                next_remaining = remaining[:i] + remaining[i+1:]
                backtrack(candidate, next_remaining, file, results)

def reconstructAllTexts(chunks: List[str]) -> List[str]:
    results = []
    file = open("found.txt", "a")

    for i, ch in enumerate(chunks):
        print(f"Trying starting chunk {i+1}/{len(chunks)}: {repr(ch)}")
        if not ch[0].isupper(): continue
        rest = chunks[:i] + chunks[i+1:]
        backtrack(ch, rest, file, results)
    
    file.close()
    return results


def main():
    global chunks
    f = open("in.txt")
    frank = f.read()
    f.close()
    chunks = [frank[i:i+16] for i in range(0, len(frank), 16)] # for external 16, for internal not found
    random.shuffle(chunks)
    #createSelfDictionary()
    createDictionary("dict_en.txt")
    results = reconstructAllTexts(chunks)
    print(results)

if __name__ == "__main__": main()