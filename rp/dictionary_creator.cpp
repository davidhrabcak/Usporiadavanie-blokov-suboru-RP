#include <iostream>
#include <fstream>
#include <queue>
#include <sstream>
#include <vector>
#include <string>
#include <unordered_map>
#include <algorithm>
using namespace std;

#define MAX_TOPLIST 10 // limits number of most frequent words for every first word

// first argument when launching program determines output file, default dictionary_custom.txt

vector<string> split_text(string sen) {
    stringstream ss(sen);
    string word;
    vector<string> words;
    
    while (ss >> word) {
        words.push_back(word);
    }
    
    return words;
}

string strip(string input) {
     string result = input;
    result.erase(remove_if(result.begin(), result.end(),
        [](unsigned char c) { return !isalnum(c); }
    ), result.end());
    return result;
}

int main(int argc, char const *argv[]) {
    vector<string> sources;
    ifstream file("input_books.txt"); // input filename
    stringstream buffer;
    buffer << file.rdbuf();
    string data = buffer.str();
    vector<string> words = split_text(data);

    unordered_map<string, unordered_map<string, int>> freq;
    for (int i = 0; i < words.size() - 1; i++) {
        freq[strip(words[i])][strip(words[i+1])]++;
    }

    unordered_map<string, vector<string>> dict;
    priority_queue<pair<int, string>> pq;

    for (auto& kv : freq) {
        for (auto& entry : kv.second) {
            pq.push(make_pair(entry.second, entry.first));
        }

        vector<string> top;
        while (!pq.empty() && top.size() < MAX_TOPLIST) {
            top.push_back(pq.top().second);
            pq.pop();
        }

        dict[kv.first] = top;
    }
    string filename = "dictionary_custom.txt";
    if (argc >= 2) {
        filename = argv[1];
    }
    ofstream out(filename);
    for (auto& kv : dict) {
        out << kv.first;
        for (string w : kv.second) {
            out << " " << w;
        }
        out << '\n';
    }

    return 0;
}
