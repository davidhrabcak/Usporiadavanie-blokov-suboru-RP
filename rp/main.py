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

def checkValidityFrequency(candidate: str, dict) -> bool:
    words = chunks.split()

    if len(words) > 2:
        words = words[1:-1]
    
    for i, w in enumerate(words):
        

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


#TODO implement text reconstruction that uses frequency dictionary
#TODO use segment.cpp for chunk creation
#TODO change reconstructAllTexts to return only one valid result
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