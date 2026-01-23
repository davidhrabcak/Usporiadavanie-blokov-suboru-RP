#from dictionary.standard_dictionary import StandardDictionary
from dictionary.two_sequence_dictionary import FrequencyDictionary
#from validation.standard_validator import StandardValidator
from validation.frequency_2_words_validator import FrequencyValidator
from dictionary.two_sequence_dictionary_creator import WordFollowerDictionary
from segment import Segmenter
from algorithm.backtrack import Backtrack

def main():
    """Executes main logic"""
    seg = Segmenter("in.txt", "chunk_file.txt", 16)
    #If needed, create chunks
    seg.segment()
    # Load chunks
    chunks = seg.get_chunks()

    # Setup dictionary and validator
        # frequency dictionary
    creator = WordFollowerDictionary(50)
    creator.run("training.txt")
    dictionary = FrequencyDictionary()
    dictionary.load("dictionary_custom.txt")
    validator = FrequencyValidator(dictionary, chunks)

    # standard
    #dictionary = StandardDictionary()
    #dictionary.load("dict_c.txt")
    #validator = StandardValidator(dictionary)
    #print("Using standard dictionary validation")

    #print(f"Dictionary size: {len(dictionary.data)} words")

    # Reconstruct texts
    reconstructor = Backtrack(validator, chunks)

    # find all results
    #results = reconstructor.reconstruct_all("found.txt")
    #print(f"Found {len(results)} valid reconstructions")
    #with open("found.txt", "a", encoding="ascii") as f:
    #    for found in results:
    #        f.write(found + "\n")

    # find the first result
    result = reconstructor.reconstruct_one("found.txt")
    if result:
        print(f"Found reconstruction: {result}")
    else:
        print("No valid reconstruction found")

if __name__ == "__main__":
    main()
