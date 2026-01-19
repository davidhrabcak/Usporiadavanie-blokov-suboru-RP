from dictionary.three_sequence_dictionary import ThreeSequenceDictionary
from validation.frequency_3_words_validator import FrequencyThreeWordsValidator

def test_validate_chunk():
    dic = ThreeSequenceDictionary()
    dic.load("test/frequency.txt", "test/coordinates.txt")
    assert "this" in dic.dictionary

    validator = FrequencyThreeWordsValidator(dic, ["This", "is", "the", "sequence",
                                                   "that", "was", "used", "for", "testing."])
    assert dic.contains("the", ["is", "sentence"])
    assert not dic.contains("was", ["used", "that"])

    assert validator.validate_chunk("the sentence ", "that was")
    assert not validator.validate_chunk("the sentence", "that was used")
    assert validator.validate_chunk("the sentence ", " that was")
    assert validator.validate_chunk("the sente", "nce that")

def main():
    test_validate_chunk()

if __name__ == "__main__":
    main()
