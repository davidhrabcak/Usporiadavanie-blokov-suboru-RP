from typing import *
import random

d = {}
chunks = []

def importFrequencyDictionary():
    with open("dictionary_custom.txt") as f: #change namefile of custom dictionary
        for line in f:
            words = line.split()
            d[words[0]] = words[1:]

def createDictionary():
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
    for chunk in chunks:
        if chunk not in current_text:
            return False
    words = current_text.split()
    return all(w.lower() in d for w in words if w.isalpha()) #words valid check

#uses default d as frequency dict
def checkValidityChunkFrequency(candidate: str) -> bool:
    words = candidate.split()

    if len(words) > 2:
        words = words[1:-1]
    
    for i, w in enumerate(words):
        if (words[i+1] not in d[w]):
            return False
    return True

#uses standard dictionary created by method of choice
def backtrack(current_text: str, remaining: List[str], file, results: List[str]):
        if not remaining:
            if checkValidityText:
                print("found!\a")
                results.append(current_text)
                file.write(f"{current_text}\n")
            return
        
        for i, ch in enumerate(remaining):
            candidate = current_text + ch
            if checkValidityChunk(candidate): # checkValidityChunkFrequency(candidate, dict)
                next_remaining = remaining[:i] + remaining[i+1:]
                backtrack(candidate, next_remaining, file, results)

def reconstructAllTexts(chunks: List[str]) -> List[str]:
    results = []
    file = open("found.txt", "a")

    for i, ch in enumerate(chunks):
        print(f"Trying starting chunk {i+1}/{len(chunks)}: {repr(ch)}")
        rest = chunks[:i] + chunks[i+1:]
        backtrack(ch, rest, file, results)
        #backtrackFrequency(ch, rest, file, results)
    
    file.close()
    return results

#TODO change reconstructAllTexts to return only one valid result
#TODO simplify and organize functions and structures for creating/importing dicts
def main():
    global chunks
    with open("out.txt") as f: #filename of segment output
        for line in f:
            chunks.append(line)
    #createSelfDictionary()
    #importFrequencyDictionary()
    createDictionary("dict_en.txt")
    results = reconstructAllTexts(chunks)
    print(results)

if __name__ == "__main__": main()