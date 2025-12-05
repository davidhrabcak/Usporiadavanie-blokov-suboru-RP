#include <fstream>
#include <queue>
#include <sstream>
#include <vector>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <set>
#include <cctype>
using namespace std;

#define MAX_TOPLIST 10 // limits number of most frequent words for every first word

// first argument when launching program determines output file, default dictionary_custom.txt


std::set<char> allowed = {'-', '\'', '_'}; // chars allowed in a word

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
    
    transform(input.begin(), input.end(), input.begin(),
    [](unsigned char c){ return std::tolower(c); });

    return input;
}

//TODO needs testing - creation and use of frequency dictionary
int main(int argc, char const *argv[]) {
    vector<string> sources;
    ifstream file("input_books.txt"); // input filename
    stringstream buffer;
    buffer << file.rdbuf();
    string data = buffer.str();
    file.close();

    vector<string> words = split_text(data);
    vector<string> words_clean;

    for (auto& word : words) {
        string clean = strip(word);
        if (!clean.empty()) words_clean.push_back(clean);
    }

    unordered_map<string, unordered_map<string, int>> freq;
    for (int i = 0; i < words_clean.size() - 1; i++) {
        freq[words_clean[i]][strip(words_clean[i+1])]++;
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
