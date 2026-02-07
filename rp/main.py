from dictionary.three_sequence_dictionary import ThreeSequenceDictionary
from dictionary.three_sequence_dict_creator import ThreeSequenceDictionaryCreator
from validation.frequency_3_words_validator import FrequencyThreeWordsValidator
from segment import Segmenter
from algorithm.backtrack import Backtrack

def main():
    """Executes main logic"""
    seg = Segmenter("in.txt", "chunk_file.txt", 64)
    seg.segment()
    chunks = seg.get_chunks()

    creator = ThreeSequenceDictionaryCreator("data.txt", "coordinates.txt")
    creator.run()
    dictionary = ThreeSequenceDictionary()
    dictionary.load("data.txt", "coordinates.txt")
    validator = FrequencyThreeWordsValidator(dictionary, chunks)

    # Reconstruct texts
    reconstructor = Backtrack(validator, chunks)
    result = reconstructor.reconstruct_all("found.txt")
    if result:
        print(f"Found reconstruction: {result}")
        print(len(result))
    else:
        print("No valid reconstruction found")

if __name__ == "__main__":
    main()
