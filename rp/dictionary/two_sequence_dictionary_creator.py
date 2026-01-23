"""used for sorting the words by their frequencies, typing method arguments,
   default dict is used to prevent missing entries errors"""
import heapq
from collections import defaultdict
from typing import List


class WordFollowerDictionary:
    """Creates 2-word frequency dictionary - for every word, it keeps the 10
       most frequent following words"""
    def __init__(self, max_toplist: int = 10):
        self.max_toplist = max_toplist

    def _build_dictionary(self, words: List[str]):
        """Internal method that builds the dictionary"""
        freq = defaultdict(lambda: defaultdict(int))

        for i in range(len(words) - 1):
            w1 = words[i]
            w2 = words[i + 1].lower()
            freq[w1][w2] += 1

        dictionary = {}

        for first_word, next_map in freq.items():
            heap = [(-count, w) for w, count in next_map.items()]
            heapq.heapify(heap)

            top_words = []
            for _ in range(min(self.max_toplist, len(heap))):
                count, w = heapq.heappop(heap)
                top_words.append(w)

            dictionary[first_word] = top_words

        return dictionary

    def run(self, input_filename: str, output_filename: str = "dictionary_custom.txt"):
        """Creates the dictionary"""

        with open(input_filename, "r") as f:
            data = f.read()

        words_raw = data.split()
        words_clean = [w.lower() for w in words_raw if w]

        result_dict = self._build_dictionary(words_clean)


        with open(output_filename, "w") as out:
            for word, followers in result_dict.items():
                out.write(word + " " + " ".join(followers) + "\n")

        return result_dict
