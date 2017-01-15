#include <cstdlib>
#include <algorithm>
#include <sstream>
#include <iostream>
#include <vector>
#include <queue>
#include <deque>
#include <limits>
#include <cmath>
#include <chrono>
#include <ctime>
#include <functional>
#include <unordered_set>

#include <unordered_map>
#include <unordered_set>

#include <math.h>
#include <execinfo.h>
#include <unistd.h>
#include <signal.h>

#include <boost/archive/text_oarchive.hpp>
#include <boost/archive/text_iarchive.hpp>
#include <boost/program_options.hpp>
#include <boost/thread.hpp>
//#include <bits/stl_deque.h>

#include "cnn/training.h"
#include "cnn/cnn.h"
#include "cnn/expr.h"
#include "cnn/nodes.h"
#include "cnn/lstm.h"
#include "cnn/rnn.h"
#include "c2.h"

/////  All this code is taken from the internet to help me find memory leaks//////////
#include "stdlib.h"
#include "stdio.h"
#include "string.h"

int parseLine(char* line){
    int i = strlen(line);
    while (*line < '0' || *line > '9') line++;
    line[i-3] = '\0';
    i = atoi(line);
    return i;
}

int getMemoryUsage(){ //Note: this value is in KB!
    FILE* file = fopen("/proc/self/status", "r");
    int result = -1;
    char line[128];


    while (fgets(line, 128, file) != NULL){
        if (strncmp(line, "VmSize:", 7) == 0){
            result = parseLine(line);
            break;
        }
    }
    fclose(file);
    return result;
}


//////////////////////////////////////////////////////////////////////////////////

cpyp::Corpus corpus;
volatile bool requested_stop = false;
unsigned LAYERS = 2;
unsigned INPUT_DIM = 40;
unsigned HIDDEN_DIM = 60;
unsigned ACTION_DIM = 36;
unsigned PRETRAINED_DIM = 50;
unsigned LSTM_INPUT_DIM = 60;
unsigned POS_DIM = 10;
unsigned REL_DIM = 8;

unsigned PARSE_DIM = 300; // Jacob


unsigned LSTM_CHAR_OUTPUT_DIM = 100; //Miguel
bool USE_SPELLING = false;

bool USE_POS = false;

bool DYNAMIC_BEAM = true;
bool GLOBAL_LOSS = false;

bool MULTITHREAD_BEAMS = false;
boost::mutex cnn_mutex;

unsigned HB_CUTOFF = 0;

constexpr const char* ROOT_SYMBOL = "ROOT";
unsigned kROOT_SYMBOL = 0;
unsigned ACTION_SIZE = 0;
unsigned VOCAB_SIZE = 0;
unsigned POS_SIZE = 0;

unsigned CHAR_SIZE = 255; //size of ascii chars... Miguel

using namespace cnn::expr;
using namespace cnn;
using namespace std;
namespace po = boost::program_options;

vector<unsigned> possible_actions;
unordered_map<unsigned, vector<float>> pretrained;

void InitCommandLine(int argc, char** argv, po::variables_map* conf) {
  po::options_description opts("Configuration options");
  opts.add_options()
        ("training_data,T", po::value<string>(), "List of Transitions - Training corpus")
        ("dev_data,d", po::value<string>(), "Development corpus")
        ("test_data,p", po::value<string>(), "Test corpus")
        ("unk_strategy,o", po::value<unsigned>()->default_value(1), "Unknown word strategy: 1 = singletons become UNK with probability unk_prob")
        ("unk_prob,u", po::value<double>()->default_value(0.2), "Probably with which to replace singletons with UNK in training data")
        ("model,m", po::value<string>(), "Load saved model from this file")
        ("use_pos_tags,P", "make POS tags visible to parser")
        ("beam_size,b", po::value<unsigned>()->default_value(0), "beam size")
        ("global_loss,G", "train using the global loss function (Andor et al)")
        ("dynamic_beam,D", "do beam search on a dynamically sized beam")
        ("multithreading", "use multithreading to speed things up (beam search)")
        ("hb_trials,B", po::value<unsigned>()->default_value(0), "decode with heuristic backtracking, this is the number of times to backtrack")
        ("selectional_margin,M", po::value<double>()->default_value(0.0), "Decode with selectional branching")
        ("layers", po::value<unsigned>()->default_value(2), "number of LSTM layers")
        ("action_dim", po::value<unsigned>()->default_value(16), "action embedding size")
        ("input_dim", po::value<unsigned>()->default_value(32), "input embedding size")
        ("hidden_dim", po::value<unsigned>()->default_value(64), "hidden dimension")
        ("pretrained_dim", po::value<unsigned>()->default_value(50), "pretrained input dimension")
        ("pos_dim", po::value<unsigned>()->default_value(12), "POS dimension")
        ("rel_dim", po::value<unsigned>()->default_value(10), "relation dimension")
        ("lstm_input_dim", po::value<unsigned>()->default_value(60), "LSTM input dimension")
        ("train,t", "Should training be run?")
        ("hb_cutoff", "Go do all attempts, without even trying to cut off?")
        ("train_hb", "Do second phase of training (cutoff)?")
        ("test_hb", "Do second phase of testing (cutoff)?")
        ("words,w", po::value<string>(), "Pretrained word embeddings")
        ("use_spelling,S", "Use spelling model") //Miguel. Spelling model
        ("help,h", "Help");
  po::options_description dcmdline_options;
  dcmdline_options.add(opts);
  po::store(parse_command_line(argc, argv, dcmdline_options), *conf);
  if (conf->count("help")) {
    cerr << dcmdline_options << endl;
    exit(1);
  }
  if (conf->count("training_data") == 0) {
    cerr << "Please specify --traing_data (-T): this is required to determine the vocabulary mapping, even if the parser is used in prediction mode.\n";
    exit(1);
  }
}

struct ParseErrorFeedback {
    unsigned guesses_correct = 0;
    unsigned guesses_incorrect = 0;
    unsigned wrong_guesses_correct = 0;
    unsigned wrong_guesses_incorrect = 0;
};

struct DataGatherer {
    unsigned sentences_parsed = 0;
    float decisions_made = 0;
    float m2_decisions_made = 0;
};

/// These functions get multithreaded


///

struct ParserBuilder {

  LSTMBuilder stack_lstm; // (layers, input, hidden, trainer)
  LSTMBuilder buffer_lstm;
  LSTMBuilder action_lstm;
  LookupParameters* p_w; // word embeddings
  LookupParameters* p_t; // pretrained word embeddings (not updated)
  LookupParameters* p_a; // input action embeddings
  LookupParameters* p_r; // relation embeddings
  LookupParameters* p_p; // pos tag embeddings
  Parameters* p_pbias; // parser state bias
  Parameters* p_A; // action lstm to parser state
  Parameters* p_B; // buffer lstm to parser state
  Parameters* p_S; // stack lstm to parser state
  Parameters* p_H; // head matrix for composition function
  Parameters* p_D; // dependency matrix for composition function
  Parameters* p_R; // relation matrix for composition function
  Parameters* p_w2l; // word to LSTM input
  Parameters* p_p2l; // POS to LSTM input
  Parameters* p_t2l; // pretrained word embeddings to LSTM input
  Parameters* p_ib; // LSTM input bias
  Parameters* p_cbias; // composition function bias
  Parameters* p_p2a;   // parser state to action
  Parameters* p_action_start;  // action bias
  Parameters* p_abias;  // action bias
  Parameters* p_buffer_guard;  // end of buffer
  Parameters* p_stack_guard;  // end of stack
  Parameters* p_hbbias;  // heuristic backtrack bias

  Parameters* p_start_of_word;//Miguel -->dummy <s> symbol
  Parameters* p_end_of_word; //Miguel --> dummy </s> symbol
  LookupParameters* char_emb; //Miguel-> mapping of characters to vectors 


  LSTMBuilder fw_char_lstm; // Miguel
  LSTMBuilder bw_char_lstm; //Miguel

  LSTMBuilder parse_lstm; // Jacob
  Parameters* p_P; // parser bias

  explicit ParserBuilder(Model* model, const unordered_map<unsigned, vector<float>>& pretrained) :
      stack_lstm(LAYERS, LSTM_INPUT_DIM, HIDDEN_DIM, model),
      buffer_lstm(LAYERS, LSTM_INPUT_DIM, HIDDEN_DIM, model),
      action_lstm(LAYERS, ACTION_DIM, HIDDEN_DIM, model),
      p_w(model->add_lookup_parameters(VOCAB_SIZE, {INPUT_DIM})),
      p_a(model->add_lookup_parameters(ACTION_SIZE, {ACTION_DIM})),
      p_r(model->add_lookup_parameters(ACTION_SIZE, {REL_DIM})),
      p_pbias(model->add_parameters({HIDDEN_DIM})),
      p_A(model->add_parameters({HIDDEN_DIM, HIDDEN_DIM})),
      p_B(model->add_parameters({HIDDEN_DIM, HIDDEN_DIM})),
      p_S(model->add_parameters({HIDDEN_DIM, HIDDEN_DIM})),
      p_H(model->add_parameters({LSTM_INPUT_DIM, LSTM_INPUT_DIM})),
      p_D(model->add_parameters({LSTM_INPUT_DIM, LSTM_INPUT_DIM})),
      p_R(model->add_parameters({LSTM_INPUT_DIM, REL_DIM})),
      p_w2l(model->add_parameters({LSTM_INPUT_DIM, INPUT_DIM})),
      p_ib(model->add_parameters({LSTM_INPUT_DIM})),
      p_cbias(model->add_parameters({LSTM_INPUT_DIM})),
      p_p2a(model->add_parameters({ACTION_SIZE, HIDDEN_DIM})),
      p_action_start(model->add_parameters({ACTION_DIM})),
      p_abias(model->add_parameters({ACTION_SIZE})),

      p_buffer_guard(model->add_parameters({LSTM_INPUT_DIM})),
      p_stack_guard(model->add_parameters({LSTM_INPUT_DIM})),

      p_start_of_word(model->add_parameters({LSTM_INPUT_DIM})), //Miguel
      p_end_of_word(model->add_parameters({LSTM_INPUT_DIM})), //Miguel 

      char_emb(model->add_lookup_parameters(CHAR_SIZE, {INPUT_DIM})),//Miguel

//      fw_char_lstm(LAYERS, LSTM_CHAR_OUTPUT_DIM, LSTM_INPUT_DIM, model), //Miguel
//      bw_char_lstm(LAYERS, LSTM_CHAR_OUTPUT_DIM, LSTM_INPUT_DIM,  model), //Miguel

      fw_char_lstm(LAYERS, LSTM_INPUT_DIM, LSTM_CHAR_OUTPUT_DIM/2, model), //Miguel 
      bw_char_lstm(LAYERS, LSTM_INPUT_DIM, LSTM_CHAR_OUTPUT_DIM/2, model), /*Miguel*/

      p_hbbias(model->add_parameters({2})), // Jacob
      p_P(model->add_parameters({2, HIDDEN_DIM})),
      parse_lstm(LAYERS, HIDDEN_DIM*3, HIDDEN_DIM, model) {
    if (USE_POS) {
      p_p = model->add_lookup_parameters(POS_SIZE, {POS_DIM});
      p_p2l = model->add_parameters({LSTM_INPUT_DIM, POS_DIM});
    }
    if (pretrained.size() > 0) {
      p_t = model->add_lookup_parameters(VOCAB_SIZE, {PRETRAINED_DIM});
      for (auto it : pretrained)
        p_t->Initialize(it.first, it.second);
      p_t2l = model->add_parameters({LSTM_INPUT_DIM, PRETRAINED_DIM});
    } else {
      p_t = nullptr;
      p_t2l = nullptr;
    }
  }

static bool IsActionForbidden(const string& a, unsigned bsize, unsigned ssize, vector<int> stacki) {
  if (a[1]=='W' && ssize<3) return true; //MIGUEL

  if (a[1]=='W') { //MIGUEL

        int top=stacki[stacki.size()-1];
        int sec=stacki[stacki.size()-2];

        if (sec>top) return true;
  }

  bool is_shift = (a[0] == 'S' && a[1]=='H');  //MIGUEL
  bool is_reduce = !is_shift;
  if (is_shift && bsize == 1) return true;
  if (is_reduce && ssize < 3) return true;
  if (bsize == 2 && // ROOT is the only thing remaining on buffer
      ssize > 2 && // there is more than a single element on the stack
      is_shift) return true;
  // only attach left to ROOT
  if (bsize == 1 && ssize == 3 && a[0] == 'R') return true;
  return false;
}

/*static bool IsActionForbidden(const string& a, unsigned bsize, unsigned ssize) {
  bool is_shift = (a[0] == 'S');
  bool is_reduce = !is_shift;
  if (is_shift && bsize == 1) return true;
  if (is_reduce && ssize < 3) return true;
  if (bsize == 2 && // ROOT is the only thing remaining on buffer
      ssize > 2 && // there is more than a single element on the stack
      is_shift) return true;
  // only attach left to ROOT
  if (bsize == 1 && ssize == 3 && a[0] == 'R') return true;
  return false;
}*/

static map<int,int> compute_heads(unsigned sent_len, const vector<unsigned>& actions, const vector<string>& setOfActions, map<int,string>* pr = nullptr) {
  map<int,int> heads;
  map<int,string> r;
  map<int,string>& rels = (pr ? *pr : r);
  for(unsigned i=0;i<sent_len;i++) { heads[i]=-1; rels[i]="ERROR"; }
  vector<int> bufferi(sent_len + 1, 0), stacki(1, -999);
  for (unsigned i = 0; i < sent_len; ++i)
    bufferi[sent_len - i] = i;
  bufferi[0] = -999;
  for (auto action: actions) { // loop over transitions for sentence
    const string& actionString=setOfActions[action];
    const char ac = actionString[0];
    const char ac2 = actionString[1];
    if (ac =='S' && ac2=='H') {  // SHIFT
      assert(bufferi.size() > 1); // dummy symbol means > 1 (not >= 1)
      stacki.push_back(bufferi.back());
      bufferi.pop_back();
    } 
   else if (ac=='S' && ac2=='W') {
        assert(stacki.size() > 2);

//	std::cout<<"SWAP"<<"\n";
        unsigned ii = 0, jj = 0;
        jj=stacki.back();
        stacki.pop_back();

        ii=stacki.back();
        stacki.pop_back();

        bufferi.push_back(ii);

        stacki.push_back(jj);
    }

    else { // LEFT or RIGHT
      assert(stacki.size() > 2); // dummy symbol means > 2 (not >= 2)
      assert(ac == 'L' || ac == 'R');
      unsigned depi = 0, headi = 0;
      (ac == 'R' ? depi : headi) = stacki.back();
      stacki.pop_back();
      (ac == 'R' ? headi : depi) = stacki.back();
      stacki.pop_back();
      stacki.push_back(headi);
      heads[depi] = headi;
      rels[depi] = actionString;
    }
  }
  assert(bufferi.size() == 1);
  //assert(stacki.size() == 2);
  return heads;
}


// given the first character of a UTF8 block, find out how wide it is
// see http://en.wikipedia.org/wiki/UTF-8 for more info
inline unsigned int UTF8Len(unsigned char x) {
  if (x < 0x80) return 1;
  else if ((x >> 5) == 0x06) return 2;
  else if ((x >> 4) == 0x0e) return 3;
  else if ((x >> 3) == 0x1e) return 4;
  else if ((x >> 2) == 0x3e) return 5;
  else if ((x >> 1) == 0x7e) return 6;
  else return 0;
}


// *** if correct_actions is empty, this runs greedy decoding ***
// returns parse actions for input sentence (in training just returns the reference)
// OOV handling: raw_sent will have the actual words
//               sent will have words replaced by appropriate UNK tokens
// this lets us use pretrained embeddings, when available, for words that were OOV in the
// parser training data
vector<unsigned> log_prob_parser(ComputationGraph* hg,
                     const vector<unsigned>& raw_sent,  // raw sentence
                     const vector<unsigned>& sent,  // sent with oovs replaced
                     const vector<unsigned>& sentPos,
                     const vector<unsigned>& correct_actions,
                     const vector<string>& setOfActions,
                     const map<unsigned, std::string>& intToWords,
                     double *right) {
//    for (unsigned i = 0; i < sent.size(); ++i) cerr << ' ' << intToWords.find(sent[i])->second;
//    cerr << endl;
    vector<unsigned> results;
    const bool build_training_graph = correct_actions.size() > 0;

    stack_lstm.new_graph(*hg);
    buffer_lstm.new_graph(*hg);
    action_lstm.new_graph(*hg);
    stack_lstm.start_new_sequence();
    buffer_lstm.start_new_sequence();
    action_lstm.start_new_sequence();
    // variables in the computation graph representing the parameters
    Expression pbias = parameter(*hg, p_pbias);
    Expression H = parameter(*hg, p_H);
    Expression D = parameter(*hg, p_D);
    Expression R = parameter(*hg, p_R);
    Expression cbias = parameter(*hg, p_cbias);
    Expression S = parameter(*hg, p_S);
    Expression B = parameter(*hg, p_B);
    Expression A = parameter(*hg, p_A);
    Expression ib = parameter(*hg, p_ib);
    Expression w2l = parameter(*hg, p_w2l);
    Expression p2l;
    if (USE_POS)
      p2l = parameter(*hg, p_p2l);
    Expression t2l;
    if (p_t2l)
      t2l = parameter(*hg, p_t2l);
    Expression p2a = parameter(*hg, p_p2a);
    Expression abias = parameter(*hg, p_abias);
    Expression action_start = parameter(*hg, p_action_start);

    action_lstm.add_input(action_start);

    vector<Expression> buffer(sent.size() + 1);  // variables representing word embeddings (possibly including POS info)
    vector<int> bufferi(sent.size() + 1);  // position of the words in the sentence
    // precompute buffer representation from left to right


    Expression word_end = parameter(*hg, p_end_of_word); //Miguel
    Expression word_start = parameter(*hg, p_start_of_word); //Miguel

    if (USE_SPELLING){
       fw_char_lstm.new_graph(*hg);
        //    fw_char_lstm.add_parameter_edges(hg);

       bw_char_lstm.new_graph(*hg);
       //    bw_char_lstm.add_parameter_edges(hg);
    }



    for (unsigned i = 0; i < sent.size(); ++i) {
      assert(sent[i] < VOCAB_SIZE);
      //Expression w = lookup(*hg, p_w, sent[i]);

      unsigned wi=sent[i];
      std::string ww=intToWords.at(wi);
      Expression w;
      /**********SPELLING MODEL*****************/
      if (USE_SPELLING) {
        //std::cout<<"using spelling"<<"\n";
        if (ww.length()==4  && ww[0]=='R' && ww[1]=='O' && ww[2]=='O' && ww[3]=='T'){
          w=lookup(*hg, p_w, sent[i]); //we do not need a LSTM encoding for the root word, so we put it directly-.
        }
        else {

            fw_char_lstm.start_new_sequence();
            //cerr<<"start_new_sequence done"<<"\n";

            fw_char_lstm.add_input(word_start);
            //cerr<<"added start of word symbol"<<"\n";
            /*for (unsigned j=0;j<w.length();j++){

                //cerr<<j<<":"<<w[j]<<"\n"; 
                Expression cj=lookup(*hg, char_emb, w[j]);
                fw_char_lstm.add_input(cj, hg);
        
               //std::cout<<"Inputdim:"<<LSTM_INPUT_DIM<<"\n";  
               //hg->incremental_forward();

            }*/
	    std::vector<int> strevbuffer;
            for (unsigned j=0;j<ww.length();j+=UTF8Len(ww[j])){

                //cerr<<j<<":"<<w[j]<<"\n"; 
                std::string wj;
                for (unsigned h=j;h<j+UTF8Len(ww[j]);h++) wj+=ww[h];
                //std::cout<<"fw"<<wj<<"\n";
                int wjint=corpus.charsToInt[wj];
		//std::cout<<"fw:"<<wjint<<"\n";
		strevbuffer.push_back(wjint);
                Expression cj=lookup(*hg, char_emb, wjint);
                fw_char_lstm.add_input(cj);

               //std::cout<<"Inputdim:"<<LSTM_INPUT_DIM<<"\n";  
               //hg->incremental_forward();

            }
            fw_char_lstm.add_input(word_end);
            //cerr<<"added end of word symbol"<<"\n";



            Expression fw_i=fw_char_lstm.back();

            //cerr<<"fw_char_lstm.back() done"<<"\n";

            bw_char_lstm.start_new_sequence();
            //cerr<<"bw start new sequence done"<<"\n";

            bw_char_lstm.add_input(word_end);
	    //for (unsigned j=w.length()-1;j>=0;j--){
            /*for (unsigned j=w.length();j-->0;){
               //cerr<<j<<":"<<w[j]<<"\n";
               Expression cj=lookup(*hg, char_emb, w[j]);
               bw_char_lstm.add_input(cj); 
            }*/

	    while(!strevbuffer.empty()) {
		int wjint=strevbuffer.back();
		//std::cout<<"bw:"<<wjint<<"\n";
		Expression cj=lookup(*hg, char_emb, wjint);
                bw_char_lstm.add_input(cj);
		strevbuffer.pop_back();
	    }
	    
            /*for (unsigned j=w.length()-1;j>0;j=j-UTF8Len(w[j])) {

                //cerr<<j<<":"<<w[j]<<"\n"; 
                std::string wj;
                for (unsigned h=j;h<j+UTF8Len(w[j]);h++) wj+=w[h];
                std::cout<<"bw"<<wj<<"\n";
                int wjint=corpus.charsToInt[wj];
                Expression cj=lookup(*hg, char_emb, wjint);
                bw_char_lstm.add_input(cj);

               //std::cout<<"Inputdim:"<<LSTM_INPUT_DIM<<"\n";  
               //hg->incremental_forward();

            }*/
            bw_char_lstm.add_input(word_start);
            //cerr<<"start symbol in bw seq"<<"\n";     

            Expression bw_i=bw_char_lstm.back();

            vector<Expression> tt = {fw_i, bw_i};
            w=concatenate(tt); //and this goes into the buffer...
            //cerr<<"fw and bw done"<<"\n";
         }

	}
      /**************************************************/
      //cerr<<"concatenate?"<<"\n";

      /***************NO SPELLING*************************************/

      // Expression w = lookup(*hg, p_w, sent[i]);
      else { //NO SPELLING
          //Don't use SPELLING
          //std::cout<<"don't use spelling"<<"\n";
          w=lookup(*hg, p_w, sent[i]);
      }

      Expression i_i;
      if (USE_POS) {
        Expression p = lookup(*hg, p_p, sentPos[i]);
        i_i = affine_transform({ib, w2l, w, p2l, p});
      } else {
        i_i = affine_transform({ib, w2l, w});
      }
      if (p_t && pretrained.count(raw_sent[i])) {
        Expression t = const_lookup(*hg, p_t, raw_sent[i]);
        i_i = affine_transform({i_i, t2l, t});
      }
      buffer[sent.size() - i] = rectify(i_i);
      bufferi[sent.size() - i] = i;
    }
    // dummy symbol to represent the empty buffer
    buffer[0] = parameter(*hg, p_buffer_guard);
    bufferi[0] = -999;
    for (auto& b : buffer)
      buffer_lstm.add_input(b);

    vector<Expression> stack;  // variables representing subtree embeddings
    vector<int> stacki; // position of words in the sentence of head of subtree
    stack.push_back(parameter(*hg, p_stack_guard));
    stacki.push_back(-999); // not used for anything
    // drive dummy symbol on stack through LSTM
    stack_lstm.add_input(stack.back());
    vector<Expression> log_probs;
    string rootword;
    unsigned action_count = 0;  // incremented at each prediction
    while(stack.size() > 2 || buffer.size() > 1) {

      // get list of possible actions for the current parser state
      vector<unsigned> current_valid_actions;
      for (auto a: possible_actions) {
        if (IsActionForbidden(setOfActions[a], buffer.size(), stack.size(), stacki))
          continue;
        current_valid_actions.push_back(a);
      }

      // p_t = pbias + S * slstm + B * blstm + A * almst
      Expression p_t = affine_transform({pbias, S, stack_lstm.back(), B, buffer_lstm.back(), A, action_lstm.back()});
      Expression nlp_t = rectify(p_t);
      // r_t = abias + p2a * nlp
      Expression r_t = affine_transform({abias, p2a, nlp_t});

      // adist = log_softmax(r_t, current_valid_actions)
      Expression adiste = log_softmax(r_t, current_valid_actions);
      vector<float> adist = as_vector(hg->incremental_forward());
      double best_score = adist[current_valid_actions[0]];
      unsigned best_a = current_valid_actions[0];
      //cerr << "current action: " << current_valid_actions[0] << " score: " << adist[current_valid_actions[0]] << "\n";
      for (unsigned i = 1; i < current_valid_actions.size(); ++i) {
        //cerr << "current action: " << current_valid_actions[i] << " score: " << adist[current_valid_actions[i]] << "\n";
        if (adist[current_valid_actions[i]] > best_score) {
          best_score = adist[current_valid_actions[i]];
          best_a = current_valid_actions[i];
        }
      }
        //cerr << "\n\n\n";
        unsigned action = best_a;
      if (build_training_graph) {  // if we have reference actions (for training) use the reference action
        action = correct_actions[action_count];
        if (best_a == action) { (*right)++; }
      }


      ++action_count;
      // action_log_prob = pick(adist, action)
      log_probs.push_back(pick(adiste, action));

       // cerr << "at count " <<action_count <<" " << " current action: " << action << " state " << stack.size() << " " << buffer.size() << "\n";

      apply_action(hg,
                   stack_lstm, buffer_lstm, action_lstm,
                   buffer, bufferi, stack, stacki, results,
                   action, setOfActions, sent, intToWords,
                   cbias, H, D, R, &rootword);

    }
       // cerr << "at count " <<action_count << " state " << stack.size() << " " << buffer.size() << "\n";

    assert(stack.size() == 2); // guard symbol, root
    assert(stacki.size() == 2);
    assert(buffer.size() == 1); // guard symbol
    assert(bufferi.size() == 1);
    Expression tot_neglogprob = -sum(log_probs);
    assert(tot_neglogprob.pg != nullptr);
    return results;
  }




// contains data structures for decoding searches

struct Action {
    unsigned val;
    double score;
    Expression log_prob;

    Expression log_zlocal;
    Expression rho;
};

struct ActionCompare {
    bool operator()(const Action& a, const Action& b) const {
        return a.score > b.score;
    }
};

struct ParserState {
  LSTMBuilder stack_lstm;
  LSTMBuilder buffer_lstm;
  LSTMBuilder action_lstm;
  vector<Expression> buffer;
  vector<int> bufferi;
  vector<Expression> stack;
  vector<int> stacki;
  vector<unsigned> results;  // sequence of predicted actions
  bool complete;

  bool gold = true;
  Action next_gold_action; // only filled for gold parses

  double score;
  vector<Expression> log_probs;
  vector<Expression> rhos;
  vector<Expression> log_zlocals;
};

struct ParserStateCompare {
    bool operator()(const ParserState& a, const ParserState& b) const {
        return a.score > b.score;
    }
};

struct ParserStatePointerCompare {
    bool operator()(ParserState* a, ParserState* b) const {
        return a->score > b->score;
    }
};

struct ParserStatePointerCompareReverse {
    bool operator()(ParserState* a, ParserState* b) const {
        return a->score < b->score;
    }
};

struct StepSelect {
    Action action;
    double total_score;
    ParserState* source;
};

struct StepSelectCompare {
    bool operator()(const StepSelect& a, const StepSelect& b) const {
        return a.total_score > b.total_score;
    }
};

struct HBNode {
    ParserState* ps;
    vector<HBNode*> history;

    LSTMBuilder parse_lstm;

    vector<Action> choices;
    unsigned next_choice_i = 0;
    bool initialized_choices = false;

    double confidence = 0;
};

struct HBNodePointerCompareError {
    bool operator()(const HBNode* a, const HBNode* b) const {
        return a->confidence > b->confidence;
    }
};

struct HBNodePointerCompareScore {
    bool operator()(const HBNode* a, const HBNode* b) const {
        return a->ps->score > b->ps->score;
    }
};



static void prune(vector<ParserState>& pq, unsigned k) {
  if (pq.size() == 1) return;
  if (k > pq.size()) k = pq.size();
  partial_sort(pq.begin(), pq.begin() + k, pq.end(), ParserStateCompare());
  pq.resize(k);
  reverse(pq.begin(), pq.end());
  //cerr << "PRUNE\n";
  //for (unsigned i = 0; i < pq.size(); ++i) {
  //  cerr << pq[i].score << endl;
  //}
}

static bool all_complete(const vector<ParserState>& pq) {
  for (auto& ps : pq) if (!ps.complete) return false;
  return true;
}

// Applies an action to a ParserState struct
void apply_action_to_state(  ComputationGraph* hg,
                             ParserState* ns,
                             unsigned action,
                             const vector<string>& setOfActions,
                             const vector<unsigned>& sent,  // sent with oovs replaced
                             const map<unsigned, std::string>& intToWords,
                             const Expression& cbias,
                             const Expression& H,
                             const Expression& D,
                             const Expression& R,
                             string* rootword) {
    apply_action(hg,
                 ns->stack_lstm, ns->buffer_lstm, ns->action_lstm,
                 ns->buffer, ns->bufferi, ns->stack, ns->stacki, ns->results,
                 action, setOfActions,
                 sent, intToWords,
                 cbias, H, D, R,
                 rootword);
}

// Applies an action (shift, reduce, etc) to a stack, buffer, and LSTM set
void apply_action( ComputationGraph* hg,
                   LSTMBuilder& stack_lstm,
                   LSTMBuilder& buffer_lstm,
                   LSTMBuilder& action_lstm,
                   vector<Expression>& buffer,
                   vector<int>& bufferi,
                   vector<Expression>& stack,
                   vector<int>& stacki,
                   vector<unsigned>& results,
                   unsigned action,
                   const vector<string>& setOfActions,
                   const vector<unsigned>& sent,  // sent with oovs replaced
                   const map<unsigned, std::string>& intToWords,
                   const Expression& cbias,
                   const Expression& H,
                   const Expression& D,
                   const Expression& R,
                   string* rootword) {

    // add current action to results
    //cerr << "add current action to results\n";
    results.push_back(action);

    // add current action to action LSTM
    //cerr << "add current action to action LSTM\n";
    Expression actione = lookup(*hg, p_a, action);
    action_lstm.add_input(actione);

    // get relation embedding from action (TODO: convert to relation from action?)
    //cerr << "get relation embedding from action\n";
    Expression relation = lookup(*hg, p_r, action);

    const string &actionString = setOfActions[action];



    const char ac = actionString[0];
    const char ac2 = actionString[1];
    // Execute one of the actions
    if (ac == 'S' && ac2 == 'H') {  // SHIFT
        assert(buffer.size() > 1); // dummy symbol means > 1 (not >= 1)
        stack.push_back(buffer.back());
        stack_lstm.add_input(buffer.back());
        buffer.pop_back();
        buffer_lstm.rewind_one_step();
        stacki.push_back(bufferi.back());
        bufferi.pop_back();
    }
    else if (ac == 'S' && ac2 == 'W') { //SWAP --- Miguel
        assert(stack.size() > 2); // dummy symbol means > 2 (not >= 2)

        //std::cout<<"SWAP: "<<"stack.size:"<<stack.size()<<"\n";

        Expression toki, tokj;
        unsigned ii = 0, jj = 0;
        tokj = stack.back();
        jj = stacki.back();
        stack.pop_back();
        stacki.pop_back();

        toki = stack.back();
        ii = stacki.back();
        stack.pop_back();
        stacki.pop_back();

        buffer.push_back(toki);
        bufferi.push_back(ii);

        stack_lstm.rewind_one_step();
        stack_lstm.rewind_one_step();


        buffer_lstm.add_input(buffer.back());

        stack.push_back(tokj);
        stacki.push_back(jj);

        stack_lstm.add_input(stack.back());

        //stack_lstm.rewind_one_step();
        //buffer_lstm.rewind_one_step();
    }
    else { // LEFT or RIGHT
        assert(stack.size() > 2); // dummy symbol means > 2 (not >= 2)
        assert(ac == 'L' || ac == 'R');
        Expression dep, head;
        unsigned depi = 0, headi = 0;
        (ac == 'R' ? dep : head) = stack.back();
        (ac == 'R' ? depi : headi) = stacki.back();
        stack.pop_back();
        stacki.pop_back();
        (ac == 'R' ? head : dep) = stack.back();
        (ac == 'R' ? headi : depi) = stacki.back();
        stack.pop_back();
        stacki.pop_back();
        if (headi == sent.size() - 1) *rootword = intToWords.find(sent[depi])->second;
        // composed = cbias + H * head + D * dep + R * relation
        Expression composed = affine_transform({cbias, H, head, D, dep, R, relation});
        Expression nlcomposed = tanh(composed);
        stack_lstm.rewind_one_step();
        stack_lstm.rewind_one_step();
        stack_lstm.add_input(nlcomposed);
        stack.push_back(nlcomposed);
        stacki.push_back(headi);
    }
}

static void add_int_to_vector(int i, std::vector<int>* v) {
    for (int j=0;j<i;j++) {v->push_back(i);}
}

struct getNextBeamsArgs {
    const vector<string>& setOfActions;
    const Expression& p2a;
    const Expression& pbias;
    const Expression& abias;
    const Expression& S;
    const Expression& B;
    const Expression& A;
    const bool& build_training_graph;
    const vector<unsigned>& correct_actions;
    const int& action_count;
};

static void getNextBeams(ParserState* cur, vector<StepSelect>* potential_next_beams,
                                ComputationGraph* hg,
				    const getNextBeamsArgs& args,
                                ParserState*& gold_parse){

    const vector<string>& setOfActions = args.setOfActions;
    const Expression& p2a = args.p2a;
    const Expression& pbias = args.pbias;
    const Expression& abias = args.abias;
    const Expression& S = args.S;
    const Expression& B = args.B;
    const Expression& A = args.A;
    const bool& build_training_graph = args.build_training_graph;
    const vector<unsigned>& correct_actions = args.correct_actions;
    const int& action_count = args.action_count;

    // get list of possible actions for the current parser state
    vector<unsigned> current_valid_actions;
    for (auto a: possible_actions) {
        if (IsActionForbidden(setOfActions[a], cur->buffer.size(), cur->stack.size(), cur->stacki))
            continue;
        current_valid_actions.push_back(a);
    }

    cnn_mutex.lock(); // multithreading doesn't actually work
    // p_t = pbias + S * slstm + B * blstm + A * almst
    Expression p_t = affine_transform({pbias, S, cur->stack_lstm.back(), B, cur->buffer_lstm.back(), A, cur->action_lstm.back()});
    Expression nlp_t = rectify(p_t);
    // r_t = abias + p2a * nlp
    Expression r_t = affine_transform({abias, p2a, nlp_t});
    // adist = log_softmax(r_t, current_valid_actions)
    Expression adiste = log_softmax(r_t, current_valid_actions);
    vector<float> adist = as_vector(hg->incremental_forward());

    Expression log_zlocal;
    if (GLOBAL_LOSS) { // not used in experiments
        vector<Expression> intermediate;
        for (unsigned valid_action_loc : current_valid_actions) { intermediate.push_back(pick(r_t, valid_action_loc)); }
        log_zlocal = logsumexp(intermediate);
    }
    for (unsigned i = 0; i < current_valid_actions.size(); ++i) {
        // For each action, its value is equal to the current state's value, plus the value of the action
        double total_score = cur->score + adist[current_valid_actions[i]];
        if (GLOBAL_LOSS) {
            total_score = cur->score + *(pick(r_t, current_valid_actions[i]).value().v);
        }

        //cerr << "filling\n";
        Action act;
        act.score = adist[current_valid_actions[i]];
        act.val = current_valid_actions[i];
        act.log_prob = pick(adiste, act.val);
        if (GLOBAL_LOSS) {
            act.log_zlocal = log_zlocal;
            act.rho = pick(r_t, act.val);
            act.score = *(act.rho.value().v);
        }
        StepSelect next_step;
        next_step.source = cur;
        next_step.action = act;
        next_step.total_score = total_score;

        // if it is gold, give the gold act
        if (build_training_graph && cur->gold) {
            Action gold_act;
            gold_act.score = adist[correct_actions[action_count]];
            gold_act.val = correct_actions[action_count];
            gold_act.log_prob = pick(adiste, gold_act.val);
            if (GLOBAL_LOSS) {
                gold_act.log_zlocal = log_zlocal;
                gold_act.rho = pick(r_t, gold_act.val);
                gold_act.score = *(gold_act.rho.value().v);
            }
            gold_parse = cur;
            gold_parse->next_gold_action = gold_act;
        }
        potential_next_beams->push_back(next_step);
    }
    cnn_mutex.unlock();
};


// run beam search
    vector<unsigned> log_prob_parser_beam(ComputationGraph *hg, const vector<unsigned> &raw_sent,
                                              const vector<unsigned> &sent, const vector<unsigned> &sentPos,
                                              const vector<unsigned> &correct_actions, const vector<string> &setOfActions,
                                              const map<unsigned, std::string> &intToWords, double *right, unsigned beam_size,
                                              DataGatherer &dg) {
        //for (unsigned i = 0; i < sent.size(); ++i) cerr << ' ' << intToWords.find(sent[i])->second;
        //cerr << endl;

        //cerr << "\nstarting with memory usage: " << getMemoryUsage() << "\n";

        auto overall_start = std::chrono::high_resolution_clock::now();

        vector<unsigned> results;
        const bool build_training_graph = correct_actions.size() > 0;

        stack_lstm.new_graph(*hg);
        buffer_lstm.new_graph(*hg);
        action_lstm.new_graph(*hg);
        stack_lstm.start_new_sequence();
        buffer_lstm.start_new_sequence();
        action_lstm.start_new_sequence();
        // variables in the computation graph representing the parameters
        Expression pbias = parameter(*hg, p_pbias);
        Expression H = parameter(*hg, p_H);
        Expression D = parameter(*hg, p_D);
        Expression R = parameter(*hg, p_R);
        Expression cbias = parameter(*hg, p_cbias);
        Expression S = parameter(*hg, p_S);
        Expression B = parameter(*hg, p_B);
        Expression A = parameter(*hg, p_A);
        Expression ib = parameter(*hg, p_ib);
        Expression w2l = parameter(*hg, p_w2l);
        Expression p2l;
        if (USE_POS)
            p2l = parameter(*hg, p_p2l);
        Expression t2l;
        if (p_t2l)
            t2l = parameter(*hg, p_t2l);
        Expression p2a = parameter(*hg, p_p2a);
        Expression abias = parameter(*hg, p_abias);
        Expression action_start = parameter(*hg, p_action_start);

        action_lstm.add_input(action_start);

        vector<Expression> buffer(sent.size() + 1);  // variables representing word embeddings (possibly including POS info)
        vector<int> bufferi(sent.size() + 1);  // position of the words in the sentence
        // precompute buffer representation from left to right


        Expression word_end = parameter(*hg, p_end_of_word); //Miguel
        Expression word_start = parameter(*hg, p_start_of_word); //Miguel

        if (USE_SPELLING){
            fw_char_lstm.new_graph(*hg);
            //    fw_char_lstm.add_parameter_edges(hg);

            bw_char_lstm.new_graph(*hg);
            //    bw_char_lstm.add_parameter_edges(hg);
        }

        for (unsigned i = 0; i < sent.size(); ++i) {
            assert(sent[i] < VOCAB_SIZE);
            //Expression w = lookup(*hg, p_w, sent[i]);

            unsigned wi=sent[i];
            std::string ww=intToWords.at(wi);
            Expression w;
            /**********SPELLING MODEL*****************/
            if (USE_SPELLING) {
                //std::cout<<"using spelling"<<"\n";
                if (ww.length()==4  && ww[0]=='R' && ww[1]=='O' && ww[2]=='O' && ww[3]=='T'){
                    w=lookup(*hg, p_w, sent[i]); //we do not need a LSTM encoding for the root word, so we put it directly-.
                }
                else {

                    fw_char_lstm.start_new_sequence();
                    //cerr<<"start_new_sequence done"<<"\n";

                    fw_char_lstm.add_input(word_start);
                    //cerr<<"added start of word symbol"<<"\n";
                    /*for (unsigned j=0;j<w.length();j++){

                        //cerr<<j<<":"<<w[j]<<"\n";
                        Expression cj=lookup(*hg, char_emb, w[j]);
                        fw_char_lstm.add_input(cj, hg);

                       //std::cout<<"Inputdim:"<<LSTM_INPUT_DIM<<"\n";
                       //hg->incremental_forward();

                    }*/
                    std::vector<int> strevbuffer;
                    for (unsigned j=0;j<ww.length();j+=UTF8Len(ww[j])){

                        //cerr<<j<<":"<<w[j]<<"\n";
                        std::string wj;
                        for (unsigned h=j;h<j+UTF8Len(ww[j]);h++) wj+=ww[h];
                        //std::cout<<"fw"<<wj<<"\n";
                        int wjint=corpus.charsToInt[wj];
                        //std::cout<<"fw:"<<wjint<<"\n";
                        strevbuffer.push_back(wjint);
                        Expression cj=lookup(*hg, char_emb, wjint);
                        fw_char_lstm.add_input(cj);

                        //std::cout<<"Inputdim:"<<LSTM_INPUT_DIM<<"\n";
                        //hg->incremental_forward();

                    }
                    fw_char_lstm.add_input(word_end);
                    //cerr<<"added end of word symbol"<<"\n";



                    Expression fw_i=fw_char_lstm.back();

                    //cerr<<"fw_char_lstm.back() done"<<"\n";

                    bw_char_lstm.start_new_sequence();
                    //cerr<<"bw start new sequence done"<<"\n";

                    bw_char_lstm.add_input(word_end);
                    //for (unsigned j=w.length()-1;j>=0;j--){
                    /*for (unsigned j=w.length();j-->0;){
                       //cerr<<j<<":"<<w[j]<<"\n";
                       Expression cj=lookup(*hg, char_emb, w[j]);
                       bw_char_lstm.add_input(cj);
                    }*/

                    while(!strevbuffer.empty()) {
                        int wjint=strevbuffer.back();
                        //std::cout<<"bw:"<<wjint<<"\n";
                        Expression cj=lookup(*hg, char_emb, wjint);
                        bw_char_lstm.add_input(cj);
                        strevbuffer.pop_back();
                    }

                    /*for (unsigned j=w.length()-1;j>0;j=j-UTF8Len(w[j])) {

                        //cerr<<j<<":"<<w[j]<<"\n";
                        std::string wj;
                        for (unsigned h=j;h<j+UTF8Len(w[j]);h++) wj+=w[h];
                        std::cout<<"bw"<<wj<<"\n";
                        int wjint=corpus.charsToInt[wj];
                        Expression cj=lookup(*hg, char_emb, wjint);
                        bw_char_lstm.add_input(cj);

                       //std::cout<<"Inputdim:"<<LSTM_INPUT_DIM<<"\n";
                       //hg->incremental_forward();

                    }*/
                    bw_char_lstm.add_input(word_start);
                    //cerr<<"start symbol in bw seq"<<"\n";

                    Expression bw_i=bw_char_lstm.back();

                    vector<Expression> tt = {fw_i, bw_i};
                    w=concatenate(tt); //and this goes into the buffer...
                    //cerr<<"fw and bw done"<<"\n";
                }

            }
                /**************************************************/
                //cerr<<"concatenate?"<<"\n";

                /***************NO SPELLING*************************************/

                // Expression w = lookup(*hg, p_w, sent[i]);
            else { //NO SPELLING
                //Don't use SPELLING
                //std::cout<<"don't use spelling"<<"\n";
                w=lookup(*hg, p_w, sent[i]);
            }

            Expression i_i;
            if (USE_POS) {
                Expression p = lookup(*hg, p_p, sentPos[i]);
                i_i = affine_transform({ib, w2l, w, p2l, p});
            } else {
                i_i = affine_transform({ib, w2l, w});
            }
            if (p_t && pretrained.count(raw_sent[i])) {
                Expression t = const_lookup(*hg, p_t, raw_sent[i]);
                i_i = affine_transform({i_i, t2l, t});
            }
            buffer[sent.size() - i] = rectify(i_i);
            bufferi[sent.size() - i] = i;
        }

        // dummy symbol to represent the empty buffer
        buffer[0] = parameter(*hg, p_buffer_guard);
        bufferi[0] = -999;
        for (auto& b : buffer)
            buffer_lstm.add_input(b);

        vector<Expression> stack;  // variables representing subtree embeddings
        vector<int> stacki; // position of words in the sentence of head of subtree
        stack.push_back(parameter(*hg, p_stack_guard));
        stacki.push_back(-999); // not used for anything
        // drive dummy symbol on stack through LSTM
        stack_lstm.add_input(stack.back());

        //
        // End of "setup code", below this is beam search code
        //

        auto step_start = std::chrono::high_resolution_clock::now();

        int newcount = 0;
        int delcount = 0;

        // initialize structures for beam search
        ParserState* init = new ParserState(); newcount ++;
        init->stack_lstm = stack_lstm;
        init->buffer_lstm = buffer_lstm;
        init->action_lstm = action_lstm;
        init->buffer = buffer;
        init->bufferi = bufferi;
        init->stack = stack;
        init->stacki = stacki;
        init->results = results;
        init->score = 0;
        init->gold = true;
        if (init->stacki.size() ==1 && init->bufferi.size() == 1) { assert(!"bad0"); }

        vector<ParserState*> ongoing; // represents the currently-active beams
        ongoing.push_back(init);
        vector<StepSelect> next_beams; // represents the "next" set of beams, to be used when all current beams are exhausted
        vector<ParserState*> completed; // contains beams that have parsed the whole sentence
        unordered_set<ParserState*> need_to_delete;
        need_to_delete.insert(init);

        auto loop_start = std::chrono::high_resolution_clock::now();
	
        double beam_acceptance_percentage;
        if (DYNAMIC_BEAM) { beam_acceptance_percentage = log((100-beam_size)/100.0); beam_size = 32; } // 32 is maximum beams for dynamic
        unsigned active_beams = beam_size; // counts the number of incomplete beams we still need to process
        string rootword;
        ParserState* gold_parse = init;
        unsigned action_count = 0;  // incremented at each prediction
        bool full_gold_found = false;
        vector<Expression> log_probs;
        vector<Expression> log_zlocals;
        vector<Expression> rhos;
        while (completed.size() < beam_size){
            if (ongoing.size() == 0) { // if we've run out of beams in the current step, start on the next one
                auto step_end = std::chrono::high_resolution_clock::now();
                double dur = std::chrono::duration<double, std::milli>(step_end - step_start).count();
                step_start = step_end;

                if (next_beams.size() == 0) {
                    // Sometimes, we have completed all the beams we can, but we set the beam size too high, and there
                    // just aren't enough unique moves to complete more. In that case, we are just done.
                    break;
                }

                // Move the next set to be the current set
                for (StepSelect st : next_beams) {
                    // Create a new ParserState, copying the current one
                    ParserState* ns = new ParserState(); newcount++;
                    need_to_delete.insert(ns); // this prevents memory leaks
                    *ns = *(st.source);

                    // Update the score
                    ns->score += st.action.score;

                    // update the goldness
                    if (build_training_graph && (!ns->gold || st.action.val != correct_actions[action_count])) ns->gold = false;

                    // action_log_prob = pick(adist, action)
                    ns->log_probs.push_back(st.action.log_prob);
                    if (GLOBAL_LOSS) {
                        ns->log_zlocals.push_back(st.action.log_zlocal);
                        ns->rhos.push_back(st.action.rho);
                    }
                    // do action
                    apply_action_to_state(hg, ns, st.action.val,
                                          setOfActions, sent, intToWords,
                                          cbias, H, D, R, &rootword);
                    ongoing.push_back(ns);
                }
                next_beams.clear();
                ++action_count;

                // if we have reference actions (for training), and are doing early-update,
                // check whether we need to cut off parsing of the sentence
                if (build_training_graph) {

                    bool gold_in_beam = full_gold_found;
                    for (ParserState* ps : ongoing) {
                        if (ps->gold) {
                            gold_in_beam = true;
                            break;
                        }
                    }
                    if (!gold_in_beam) {
                        Action gold_action = gold_parse->next_gold_action;

                        gold_parse->score += gold_action.score;
                        // action_log_prob = pick(adist, action)
                        gold_parse->log_probs.push_back(gold_action.log_prob);
                        if (GLOBAL_LOSS) {
                            gold_parse->log_zlocals.push_back(gold_action.log_zlocal);
                            gold_parse->rhos.push_back(gold_action.rho);
                        }

                        // is this necessary?
                        apply_action_to_state(hg, gold_parse, gold_action.val,
                                              setOfActions, sent, intToWords,
                                              cbias, H, D, R, &rootword);
                        break;
                    }
                }
            }


            // define a couple of data structures to parallelize
            // NOTE - this didn't end up working
            vector<boost::thread*> threadz;
            vector<vector<StepSelect>*> next_beam_array;
            while (ongoing.size() != 0) {
                // get the state of a beam, and remove that beam from ongoing (because it has been processed)
                ParserState *cur = ongoing.back();
                need_to_delete.insert(cur); // this prevents memory leaks
                ongoing.pop_back();
                //cerr << "sc2 " << ongoing.top()->score << "\n";

                // check whether the current beam is completed
                if (cur->stack.size() == 2 && cur->buffer.size() == 1) {
                    completed.push_back(cur);
                    if (cur->gold) {
                        gold_parse = cur;
                        full_gold_found = true;
                    }
                    --active_beams;
                    if (completed.size() == beam_size)
                        break; // we have completed all the beams we need, so just end here
                    continue;
                }
                // Since we have now confirmed that the beam is not complete, we want to generate all possible actions to
                // take from here, and keep the best states for the next beam set
                dg.decisions_made++;
                getNextBeamsArgs nba{setOfActions,p2a,pbias,abias,S,B,A,build_training_graph,correct_actions,action_count};
                if (MULTITHREAD_BEAMS && !(cur->gold)) {
                    unsigned index = ongoing.size()-1;
		      vector<StepSelect>* potential_next_beams = new vector<StepSelect>();
                    boost::thread* nt = new boost::thread{&ParserBuilder::getNextBeams,cur, potential_next_beams,
												     hg,
												     nba,
												     gold_parse};
                    threadz.push_back(nt);
                    next_beam_array.push_back(potential_next_beams);
                }
                else {
                    vector<StepSelect> potential_next_beams;
		      getNextBeams(cur, &potential_next_beams,
                                 hg,
                                 nba,
                                 gold_parse);
                    next_beams.insert(next_beams.end(), potential_next_beams.begin(), potential_next_beams.end());
                }
            }
            if (MULTITHREAD_BEAMS && next_beam_array.size() > 0) {
                while (threadz.size() > 0) { threadz.back()->join(); threadz.pop_back(); }

                for (vector<StepSelect>* potential_next_beams : next_beam_array) {
                    next_beams.insert(next_beams.end(), potential_next_beams->begin(), potential_next_beams->end());
                }
            }
            // cull down next_beams to just keep the best beams
            // keep the next_beams sorted
            sort(next_beams.begin(), next_beams.end(), StepSelectCompare());
            if (DYNAMIC_BEAM) {
		if (next_beams.size() > 0) {
                while ((next_beams.back()).total_score <
                       (next_beams.front()).total_score + beam_acceptance_percentage ||
                       next_beams.size() > beam_size) {
                    next_beams.pop_back();
                }
              }
            } else {
                while (next_beams.size() > active_beams) {
                    next_beams.pop_back();
                }
            }
        }
        auto got_answers = std::chrono::high_resolution_clock::now();
        // if we are training, just use the gold one
        if (build_training_graph) {
            stack_lstm = gold_parse->stack_lstm;
            buffer_lstm = gold_parse->buffer_lstm;
            action_lstm = gold_parse->action_lstm;
            stack = gold_parse->stack;
            stacki = gold_parse->stacki;
            buffer = gold_parse->buffer;
            bufferi = gold_parse->bufferi;
            results = gold_parse->results;
            log_probs = gold_parse->log_probs;
            if (GLOBAL_LOSS) {
                log_zlocals = gold_parse->log_zlocals;
                rhos = gold_parse->rhos;
            }
            // Count how many actions we got right
            assert(results.size() <= correct_actions.size());
            for (unsigned i = 0; i < results.size(); i++) {
                if (correct_actions[i] == results[i]) { (*right)++; }
            }
        } else { // if we don't have answers, just take the results from the best beam
            sort(completed.begin(), completed.end(), ParserStatePointerCompare());

            stack_lstm = completed.front()->stack_lstm;
            buffer_lstm = completed.front()->buffer_lstm;
            action_lstm = completed.front()->action_lstm;
            stack = completed.front()->stack;
            stacki = completed.front()->stacki;
            buffer = completed.front()->buffer;
            bufferi = completed.front()->bufferi;
            results = completed.front()->results;
            log_probs = completed.front()->log_probs;
            if (GLOBAL_LOSS) {
                log_zlocals = completed.front()->log_zlocals;
                rhos = completed.front()->rhos;
            }

            assert(stack.size() == 2); // guard symbol, root
            assert(stacki.size() == 2);
            assert(buffer.size() == 1); // guard symbol
            assert(bufferi.size() == 1);

            auto overall_end = std::chrono::high_resolution_clock::now();
        }

        Expression intermediate_loss;
        if (GLOBAL_LOSS && build_training_graph) {
            // Global loss from Andor et al. 2016
            // NOTE - this did not end up working
            vector<Expression> beam_sum_rhos;
            vector<Expression> beam_exp_sum_log_probs;
            vector<Expression> beam_sum_log_probs; // sum(beam_sum_log_pLI) = log(pL)
            vector<Expression> beam_sum_log_probs2; // sum(beam_sum_log_pLI) = log(pL)

            if (completed.size() < beam_size) { // Bj, all ongoing beams and the gold beam
                for (ParserState *ps : ongoing) { beam_sum_rhos.push_back(sum(ps->rhos)); }
                beam_sum_rhos.push_back(sum(rhos)); // gold beam

                for (ParserState *ps : ongoing) { beam_exp_sum_log_probs.push_back(exp(sum(ps->log_probs))); }
                beam_exp_sum_log_probs.push_back(exp(sum(log_probs))); // gold beam
		
                for (ParserState *ps : ongoing) { beam_sum_log_probs.push_back(sum(ps->log_probs)); }
                beam_sum_log_probs.push_back(sum(log_probs)); // gold beam

                for (ParserState *ps : ongoing) {
                    vector<Expression> log_pLIs;
                    for (unsigned act_i = 0; act_i < ps->rhos.size(); act_i++) {
                        log_pLIs.push_back(ps->rhos[act_i] - ps->log_zlocals[act_i]);
                    }
                    beam_sum_log_probs2.push_back(sum(log_pLIs));
                }
                vector<Expression> log_pLIs;
                for (unsigned act_i = 0; act_i < rhos.size(); act_i++) {
                     log_pLIs.push_back(rhos[act_i] - log_zlocals[act_i]);
                }
                beam_sum_log_probs2.push_back(sum(log_pLIs)); // gold beam
            } else { // Bn, set of completed beams
                assert(completed.size() == beam_size);
                for (ParserState* ps : completed) { beam_sum_rhos.push_back(sum(ps->rhos)); }
                for (ParserState* ps : completed) { beam_exp_sum_log_probs.push_back(exp(sum(ps->log_probs))); }
                for (ParserState* ps : completed) { beam_sum_log_probs.push_back(sum(ps->log_probs)); }
                for (ParserState* ps : completed) {
                    vector<Expression> log_pLIs;
                    for (unsigned act_i = 0; act_i < ps->rhos.size(); act_i++) {
                        log_pLIs.push_back(ps->rhos[act_i] - ps->log_zlocals[act_i]);
                    }
                    beam_sum_log_probs2.push_back(sum(log_pLIs));
                }
            }

//            cerr << "rhos:       \t"; for (Expression rho : rhos) { cerr << rho.value() << "\t";}  cerr << "\n";
//            cerr << "beam zg:    \t"; for (Expression beam_zglobal : beam_zglobals) { cerr << beam_zglobal.value() << "\n\t\t";}  cerr << "\n";
//
//            cerr << "-sum(rhos) ?= -sum(log_probs) - sum(log_zlocals): \n";
//            cerr << (-sum(rhos)).value() << " ?= " << (-sum(log_probs)-sum(log_zlocals)).value() << " (" << (-sum(log_probs)).value() << " + " << (-sum(log_zlocals)).value() << ")\n";
//
//            cerr << (-sum(rhos)).value() <<  " + " << logsumexp(beam_zglobals).value() << "\n";

            cerr << std::setprecision(10);

            vector<Expression> log_pLIs;            
            vector<Expression> lil_rhos;
            vector<Expression> lil_bslp;
            for (unsigned act_i = 0; act_i < rhos.size(); act_i++) {
                log_pLIs.push_back(rhos[act_i] - log_zlocals[act_i]);
	    }
            intermediate_loss = -sum(rhos) + logsumexp(beam_sum_rhos);

	    
        } else {
            intermediate_loss = -sum(log_probs);
        }

        // prevents memory leaks
        ongoing.clear();
        next_beams.clear();
        completed.clear();
        for (ParserState* ps: need_to_delete) {delete ps; delcount++;}
        need_to_delete.clear();


        dg.sentences_parsed++;
        Expression tot_neglogprob;
        tot_neglogprob = intermediate_loss;
        assert(tot_neglogprob.pg != nullptr);
        return results;
    }





// run selectional branching search
    vector<unsigned> log_prob_parser_sb(ComputationGraph *hg, const vector<unsigned> &raw_sent,
                                        const vector<unsigned> &sent, const vector<unsigned> &sentPos,
                                        const vector<unsigned> &correct_actions, const vector<string> &setOfActions,
                                        const map<unsigned, std::string> &intToWords, unsigned beam_count,
                                        double selectional_margin, DataGatherer &dg) {
        //for (unsigned i = 0; i < sent.size(); ++i) cerr << ' ' << intToWords.find(sent[i])->second;
        //cerr << endl;
        vector<unsigned> results;
        const bool build_training_graph = correct_actions.size() > 0;
        stack_lstm.new_graph(*hg);
        buffer_lstm.new_graph(*hg);
        action_lstm.new_graph(*hg);
        stack_lstm.start_new_sequence();
        buffer_lstm.start_new_sequence();
        action_lstm.start_new_sequence();
        // variables in the computation graph representing the parameters
        Expression pbias = parameter(*hg, p_pbias);
        Expression H = parameter(*hg, p_H);
        Expression D = parameter(*hg, p_D);
        Expression R = parameter(*hg, p_R);
        Expression cbias = parameter(*hg, p_cbias);
        Expression S = parameter(*hg, p_S);
        Expression B = parameter(*hg, p_B);
        Expression A = parameter(*hg, p_A);
        Expression ib = parameter(*hg, p_ib);
        Expression w2l = parameter(*hg, p_w2l);
        Expression p2l;
        if (USE_POS)
            p2l = parameter(*hg, p_p2l);
        Expression t2l;
        if (p_t2l)
            t2l = parameter(*hg, p_t2l);
        Expression p2a = parameter(*hg, p_p2a);
        Expression abias = parameter(*hg, p_abias);
        Expression action_start = parameter(*hg, p_action_start);

        action_lstm.add_input(action_start);

        vector<Expression> buffer(sent.size() + 1);  // variables representing word embeddings (possibly including POS info)
        vector<int> bufferi(sent.size() + 1);  // position of the words in the sentence
        // precompute buffer representation from left to right


        Expression word_end = parameter(*hg, p_end_of_word); //Miguel
        Expression word_start = parameter(*hg, p_start_of_word); //Miguel

        if (USE_SPELLING){
            fw_char_lstm.new_graph(*hg);
            //    fw_char_lstm.add_parameter_edges(hg);

            bw_char_lstm.new_graph(*hg);
            //    bw_char_lstm.add_parameter_edges(hg);
        }

        for (unsigned i = 0; i < sent.size(); ++i) {
            assert(sent[i] < VOCAB_SIZE);
            //Expression w = lookup(*hg, p_w, sent[i]);

            unsigned wi=sent[i];
            std::string ww=intToWords.at(wi);
            Expression w;
            /**********SPELLING MODEL*****************/
            if (USE_SPELLING) {
                //std::cout<<"using spelling"<<"\n";
                if (ww.length()==4  && ww[0]=='R' && ww[1]=='O' && ww[2]=='O' && ww[3]=='T'){
                    w=lookup(*hg, p_w, sent[i]); //we do not need a LSTM encoding for the root word, so we put it directly-.
                }
                else {

                    fw_char_lstm.start_new_sequence();
                    //cerr<<"start_new_sequence done"<<"\n";

                    fw_char_lstm.add_input(word_start);
                    //cerr<<"added start of word symbol"<<"\n";
                    /*for (unsigned j=0;j<w.length();j++){

                        //cerr<<j<<":"<<w[j]<<"\n";
                        Expression cj=lookup(*hg, char_emb, w[j]);
                        fw_char_lstm.add_input(cj, hg);

                       //std::cout<<"Inputdim:"<<LSTM_INPUT_DIM<<"\n";
                       //hg->incremental_forward();

                    }*/
                    std::vector<int> strevbuffer;
                    for (unsigned j=0;j<ww.length();j+=UTF8Len(ww[j])){

                        //cerr<<j<<":"<<w[j]<<"\n";
                        std::string wj;
                        for (unsigned h=j;h<j+UTF8Len(ww[j]);h++) wj+=ww[h];
                        //std::cout<<"fw"<<wj<<"\n";
                        int wjint=corpus.charsToInt[wj];
                        //std::cout<<"fw:"<<wjint<<"\n";
                        strevbuffer.push_back(wjint);
                        Expression cj=lookup(*hg, char_emb, wjint);
                        fw_char_lstm.add_input(cj);

                        //std::cout<<"Inputdim:"<<LSTM_INPUT_DIM<<"\n";
                        //hg->incremental_forward();

                    }
                    fw_char_lstm.add_input(word_end);
                    //cerr<<"added end of word symbol"<<"\n";



                    Expression fw_i=fw_char_lstm.back();

                    //cerr<<"fw_char_lstm.back() done"<<"\n";

                    bw_char_lstm.start_new_sequence();
                    //cerr<<"bw start new sequence done"<<"\n";

                    bw_char_lstm.add_input(word_end);
                    //for (unsigned j=w.length()-1;j>=0;j--){
                    /*for (unsigned j=w.length();j-->0;){
                       //cerr<<j<<":"<<w[j]<<"\n";
                       Expression cj=lookup(*hg, char_emb, w[j]);
                       bw_char_lstm.add_input(cj);
                    }*/

                    while(!strevbuffer.empty()) {
                        int wjint=strevbuffer.back();
                        //std::cout<<"bw:"<<wjint<<"\n";
                        Expression cj=lookup(*hg, char_emb, wjint);
                        bw_char_lstm.add_input(cj);
                        strevbuffer.pop_back();
                    }

                    /*for (unsigned j=w.length()-1;j>0;j=j-UTF8Len(w[j])) {

                        //cerr<<j<<":"<<w[j]<<"\n";
                        std::string wj;
                        for (unsigned h=j;h<j+UTF8Len(w[j]);h++) wj+=w[h];
                        std::cout<<"bw"<<wj<<"\n";
                        int wjint=corpus.charsToInt[wj];
                        Expression cj=lookup(*hg, char_emb, wjint);
                        bw_char_lstm.add_input(cj);

                       //std::cout<<"Inputdim:"<<LSTM_INPUT_DIM<<"\n";
                       //hg->incremental_forward();

                    }*/
                    bw_char_lstm.add_input(word_start);
                    //cerr<<"start symbol in bw seq"<<"\n";

                    Expression bw_i=bw_char_lstm.back();

                    vector<Expression> tt = {fw_i, bw_i};
                    w=concatenate(tt); //and this goes into the buffer...
                    //cerr<<"fw and bw done"<<"\n";
                }

            }
                /**************************************************/
                //cerr<<"concatenate?"<<"\n";

                /***************NO SPELLING*************************************/

                // Expression w = lookup(*hg, p_w, sent[i]);
            else { //NO SPELLING
                //Don't use SPELLING
                //std::cout<<"don't use spelling"<<"\n";
                w=lookup(*hg, p_w, sent[i]);
            }

            Expression i_i;
            if (USE_POS) {
                Expression p = lookup(*hg, p_p, sentPos[i]);
                i_i = affine_transform({ib, w2l, w, p2l, p});
            } else {
                i_i = affine_transform({ib, w2l, w});
            }
            if (p_t && pretrained.count(raw_sent[i])) {
                Expression t = const_lookup(*hg, p_t, raw_sent[i]);
                i_i = affine_transform({i_i, t2l, t});
            }
            buffer[sent.size() - i] = rectify(i_i);
            bufferi[sent.size() - i] = i;
        }

        // dummy symbol to represent the empty buffer
        buffer[0] = parameter(*hg, p_buffer_guard);
        bufferi[0] = -999;
        for (auto& b : buffer)
            buffer_lstm.add_input(b);

        vector<Expression> stack;  // variables representing subtree embeddings
        vector<int> stacki; // position of words in the sentence of head of subtree
        stack.push_back(parameter(*hg, p_stack_guard));
        stacki.push_back(-999); // not used for anything
        // drive dummy symbol on stack through LSTM
        stack_lstm.add_input(stack.back());

        //
        // End of "setup code", below this is heuristic backtracking code
        //

        //int newhbs = 0; int newps = 0;

        // initialize structures for beam search
        ParserState* init = new ParserState(); //newps++;
        init->stack_lstm = stack_lstm;
        init->buffer_lstm = buffer_lstm;
        init->action_lstm = action_lstm;
        init->buffer = buffer;
        init->bufferi = bufferi;
        init->stack = stack;
        init->stacki = stacki;
        init->results = results;
        init->score = 0;
        init->gold = true;
        if (init->stacki.size() ==1 && init->bufferi.size() == 1) { assert(!"bad0"); }

        HBNode* start = new HBNode(); //newhbs++; cerr << newhbs << " made\n";
        start->ps = init;
        start->parse_lstm = parse_lstm;
        // fill in the choices for start

        vector<HBNode*> low_confidence_nodes; // nodes that we have processed already and can still change our minds about
        vector<HBNode*> other_nodes; // nodes that we need to remember to destroy
        vector<HBNode*> completed_nodes; // nodes that are final nodes for the sentence
        bool first_iter = true;

        vector<Expression> log_probs;
        string rootword;
        HBNode* ans;
        // while we are still searching:
        while (completed_nodes.size() < beam_count){
            // while we have not reached a finish-state:
            HBNode* cur = start;
            double base_score_penalty = start->confidence;
            while (!(cur->ps->stack.size() == 2 && cur->ps->buffer.size() == 1)) {
                // if the current node's action-choices haven't been calculated, do so
                if (!cur->initialized_choices) {
                    // get list of possible actions for the current parser state
                    vector<unsigned> current_valid_actions;
                    for (auto a: possible_actions) {
                        if (IsActionForbidden(setOfActions[a], cur->ps->buffer.size(), cur->ps->stack.size(), cur->ps->stacki))
                            continue;
                        current_valid_actions.push_back(a);
                    }
                    dg.decisions_made++;
                    // p_t = pbias + S * slstm + B * blstm + A * almst
                    Expression p_t = affine_transform({pbias, S, cur->ps->stack_lstm.back(), B, cur->ps->buffer_lstm.back(), A, cur->ps->action_lstm.back()});
                    Expression nlp_t = rectify(p_t);
                    // r_t = abias + p2a * nlp
                    Expression r_t = affine_transform({abias, p2a, nlp_t});
                    // adist = log_softmax(r_t, current_valid_actions)
                    Expression adiste = log_softmax(r_t, current_valid_actions);
                    vector<float> adist = as_vector(hg->incremental_forward());
                    for (unsigned i = 0; i < current_valid_actions.size(); ++i) {
                        Action action;
                        action.score = adist[current_valid_actions[i]];
                        action.val = current_valid_actions[i];
                        cur->choices.push_back(action);
                    }

                    sort(cur->choices.begin(), cur->choices.end(), ActionCompare());
                    cur->initialized_choices = true;
                }
                // select the next action to try
                Action next_action = cur->choices[cur->next_choice_i];
                cur->next_choice_i++;

                if (cur->next_choice_i < cur->choices.size()) {
                    cur->confidence = cur->choices[cur->next_choice_i].score;
                    double confidence = cur->choices[0].score - cur->choices[cur->next_choice_i].score;
                    if (first_iter && confidence < selectional_margin) low_confidence_nodes.push_back(cur);
                    else other_nodes.push_back(cur);
                }
                else other_nodes.push_back(cur);

                // create a new node, copying this one's parser state
                HBNode* nhb = new HBNode(); //newhbs++; cerr << newhbs << " made\n";
                nhb->ps = new ParserState(); //newps++;
                *(nhb->ps) = *(cur->ps);
                nhb->parse_lstm = cur->parse_lstm;
                nhb->history = cur->history;
                nhb->history.push_back(cur);

                //cerr << "at count " <<nhb->history.size() <<" " << " current action: " << next_action.val << " state " << nhb->ps->stack.size() << " " << nhb->ps->buffer.size() << "\n";

                // execute the choice on the new node
                apply_action_to_state(hg, nhb->ps, next_action.val,
                                      setOfActions, sent, intToWords,
                                      cbias, H, D, R, &rootword);
                nhb->ps->score += next_action.score;
                // move to the newly-created node
                cur = nhb;
            }
            // this is a potential answer
            ans = cur;
            completed_nodes.push_back(ans); //cerr << active_nodes.size()+completed_nodes.size() << "accounted for\n";

            if (first_iter) {
                first_iter = false;
                sort(low_confidence_nodes.begin(), low_confidence_nodes.end(), HBNodePointerCompareError());
                reverse(low_confidence_nodes.begin(), low_confidence_nodes.end());
            }

            if (low_confidence_nodes.size() == 0) break;

            // choose the error node (the starting point of the next search)
            start = low_confidence_nodes.back();
            low_confidence_nodes.pop_back();

        }

        sort(completed_nodes.begin(), completed_nodes.end(), HBNodePointerCompareScore());
        ans = completed_nodes.front();

	// cerr << "Completed size: " << completed_nodes.size() << "\n"; 

        // sort completed parses and take the best answer
        results = ans->ps->results;
        stack_lstm = ans->ps->stack_lstm;
        buffer_lstm = ans->ps->buffer_lstm;
        action_lstm = ans->ps->action_lstm;
        stack = ans->ps->stack;
        stacki = ans->ps->stacki;
        buffer = ans->ps->buffer;
        bufferi = ans->ps->bufferi;

        // cleanup
        for (HBNode* node : low_confidence_nodes) {
            delete node->ps; //newps--;
            node->history.clear();
            delete node; //newhbs--;
        }
        for (HBNode* node : other_nodes) {
            delete node->ps; //newps--;
            node->history.clear();
            delete node; //newhbs--;
        }
        for (HBNode* node : completed_nodes) {
            delete node->ps; //newps--;
            node->history.clear();
            delete node; //newhbs--;
        }
        low_confidence_nodes.clear();
        other_nodes.clear();
        completed_nodes.clear();


        if (HB_CUTOFF) {
            Expression tot_neglogprob = -sum(log_probs);
            assert(tot_neglogprob.pg != nullptr);
        }

        dg.sentences_parsed++;

        return results;
    }


// run heuristic backtracking search
    vector<unsigned> log_prob_parser_hb(ComputationGraph *hg, const vector<unsigned> &raw_sent,
                                            const vector<unsigned> &sent, const vector<unsigned> &sentPos,
                                            const vector<unsigned> &correct_actions, const vector<string> &setOfActions,
                                            const map<unsigned, std::string> &intToWords, double *right, unsigned *tries,
                                            unsigned max_searches, ParseErrorFeedback &pef, DataGatherer &dg) {
    //for (unsigned i = 0; i < sent.size(); ++i) cerr << ' ' << intToWords.find(sent[i])->second;
    //cerr << endl;
    vector<unsigned> results;
    const bool build_training_graph = correct_actions.size() > 0;
    stack_lstm.new_graph(*hg);
    buffer_lstm.new_graph(*hg);
    action_lstm.new_graph(*hg);
    stack_lstm.start_new_sequence();
    buffer_lstm.start_new_sequence();
    action_lstm.start_new_sequence();
    // variables in the computation graph representing the parameters
    Expression pbias = parameter(*hg, p_pbias);
    Expression H = parameter(*hg, p_H);
    Expression D = parameter(*hg, p_D);
    Expression R = parameter(*hg, p_R);
    Expression cbias = parameter(*hg, p_cbias);
    Expression S = parameter(*hg, p_S);
    Expression B = parameter(*hg, p_B);
    Expression A = parameter(*hg, p_A);
    Expression ib = parameter(*hg, p_ib);
    Expression w2l = parameter(*hg, p_w2l);
    Expression p2l;
    if (USE_POS)
        p2l = parameter(*hg, p_p2l);
    Expression t2l;
    if (p_t2l)
        t2l = parameter(*hg, p_t2l);
    Expression p2a = parameter(*hg, p_p2a);
    Expression abias = parameter(*hg, p_abias);
    Expression action_start = parameter(*hg, p_action_start);

    // Jacob
    Expression P = parameter(*hg, p_P);
    Expression hbbias = parameter(*hg, p_hbbias);
    if (HB_CUTOFF) {
        parse_lstm.new_graph(*hg);
        parse_lstm.start_new_sequence();
    }

    action_lstm.add_input(action_start);

    vector<Expression> buffer(sent.size() + 1);  // variables representing word embeddings (possibly including POS info)
    vector<int> bufferi(sent.size() + 1);  // position of the words in the sentence
    // precompute buffer representation from left to right


    Expression word_end = parameter(*hg, p_end_of_word); //Miguel
    Expression word_start = parameter(*hg, p_start_of_word); //Miguel

    if (USE_SPELLING){
        fw_char_lstm.new_graph(*hg);
        //    fw_char_lstm.add_parameter_edges(hg);

        bw_char_lstm.new_graph(*hg);
        //    bw_char_lstm.add_parameter_edges(hg);
    }

    for (unsigned i = 0; i < sent.size(); ++i) {
        assert(sent[i] < VOCAB_SIZE);
        //Expression w = lookup(*hg, p_w, sent[i]);

        unsigned wi=sent[i];
        std::string ww=intToWords.at(wi);
        Expression w;
        /**********SPELLING MODEL*****************/
        if (USE_SPELLING) {
            //std::cout<<"using spelling"<<"\n";
            if (ww.length()==4  && ww[0]=='R' && ww[1]=='O' && ww[2]=='O' && ww[3]=='T'){
                w=lookup(*hg, p_w, sent[i]); //we do not need a LSTM encoding for the root word, so we put it directly-.
            }
            else {

                fw_char_lstm.start_new_sequence();
                //cerr<<"start_new_sequence done"<<"\n";

                fw_char_lstm.add_input(word_start);
                //cerr<<"added start of word symbol"<<"\n";
                /*for (unsigned j=0;j<w.length();j++){

                    //cerr<<j<<":"<<w[j]<<"\n";
                    Expression cj=lookup(*hg, char_emb, w[j]);
                    fw_char_lstm.add_input(cj, hg);

                   //std::cout<<"Inputdim:"<<LSTM_INPUT_DIM<<"\n";
                   //hg->incremental_forward();

                }*/
                std::vector<int> strevbuffer;
                for (unsigned j=0;j<ww.length();j+=UTF8Len(ww[j])){

                    //cerr<<j<<":"<<w[j]<<"\n";
                    std::string wj;
                    for (unsigned h=j;h<j+UTF8Len(ww[j]);h++) wj+=ww[h];
                    //std::cout<<"fw"<<wj<<"\n";
                    int wjint=corpus.charsToInt[wj];
                    //std::cout<<"fw:"<<wjint<<"\n";
                    strevbuffer.push_back(wjint);
                    Expression cj=lookup(*hg, char_emb, wjint);
                    fw_char_lstm.add_input(cj);

                    //std::cout<<"Inputdim:"<<LSTM_INPUT_DIM<<"\n";
                    //hg->incremental_forward();

                }
                fw_char_lstm.add_input(word_end);
                //cerr<<"added end of word symbol"<<"\n";



                Expression fw_i=fw_char_lstm.back();

                //cerr<<"fw_char_lstm.back() done"<<"\n";

                bw_char_lstm.start_new_sequence();
                //cerr<<"bw start new sequence done"<<"\n";

                bw_char_lstm.add_input(word_end);
                //for (unsigned j=w.length()-1;j>=0;j--){
                /*for (unsigned j=w.length();j-->0;){
                   //cerr<<j<<":"<<w[j]<<"\n";
                   Expression cj=lookup(*hg, char_emb, w[j]);
                   bw_char_lstm.add_input(cj);
                }*/

                while(!strevbuffer.empty()) {
                    int wjint=strevbuffer.back();
                    //std::cout<<"bw:"<<wjint<<"\n";
                    Expression cj=lookup(*hg, char_emb, wjint);
                    bw_char_lstm.add_input(cj);
                    strevbuffer.pop_back();
                }

                /*for (unsigned j=w.length()-1;j>0;j=j-UTF8Len(w[j])) {

                    //cerr<<j<<":"<<w[j]<<"\n";
                    std::string wj;
                    for (unsigned h=j;h<j+UTF8Len(w[j]);h++) wj+=w[h];
                    std::cout<<"bw"<<wj<<"\n";
                    int wjint=corpus.charsToInt[wj];
                    Expression cj=lookup(*hg, char_emb, wjint);
                    bw_char_lstm.add_input(cj);

                   //std::cout<<"Inputdim:"<<LSTM_INPUT_DIM<<"\n";
                   //hg->incremental_forward();

                }*/
                bw_char_lstm.add_input(word_start);
                //cerr<<"start symbol in bw seq"<<"\n";

                Expression bw_i=bw_char_lstm.back();

                vector<Expression> tt = {fw_i, bw_i};
                w=concatenate(tt); //and this goes into the buffer...
                //cerr<<"fw and bw done"<<"\n";
            }

        }
            /**************************************************/
            //cerr<<"concatenate?"<<"\n";

            /***************NO SPELLING*************************************/

            // Expression w = lookup(*hg, p_w, sent[i]);
        else { //NO SPELLING
            //Don't use SPELLING
            //std::cout<<"don't use spelling"<<"\n";
            w=lookup(*hg, p_w, sent[i]);
        }

        Expression i_i;
        if (USE_POS) {
            Expression p = lookup(*hg, p_p, sentPos[i]);
            i_i = affine_transform({ib, w2l, w, p2l, p});
        } else {
            i_i = affine_transform({ib, w2l, w});
        }
        if (p_t && pretrained.count(raw_sent[i])) {
            Expression t = const_lookup(*hg, p_t, raw_sent[i]);
            i_i = affine_transform({i_i, t2l, t});
        }
        buffer[sent.size() - i] = rectify(i_i);
        bufferi[sent.size() - i] = i;
    }

    // dummy symbol to represent the empty buffer
    buffer[0] = parameter(*hg, p_buffer_guard);
    bufferi[0] = -999;
    for (auto& b : buffer)
        buffer_lstm.add_input(b);

    vector<Expression> stack;  // variables representing subtree embeddings
    vector<int> stacki; // position of words in the sentence of head of subtree
    stack.push_back(parameter(*hg, p_stack_guard));
    stacki.push_back(-999); // not used for anything
    // drive dummy symbol on stack through LSTM
    stack_lstm.add_input(stack.back());

    // Jacob
    if (HB_CUTOFF) {
        vector<Expression> parseembedding = {stack_lstm.back(), buffer_lstm.back(), action_lstm.back()};
        Expression pemb = concatenate(parseembedding);
        Expression cutoff_pemb = nobackprop(pemb);
        parse_lstm.add_input(cutoff_pemb);
    }

    //
    // End of "setup code", below this is heuristic backtracking code
    //

    // initialize structures for beam search
    ParserState* init = new ParserState(); //newps++;
    init->stack_lstm = stack_lstm;
    init->buffer_lstm = buffer_lstm;
    init->action_lstm = action_lstm;
    init->buffer = buffer;
    init->bufferi = bufferi;
    init->stack = stack;
    init->stacki = stacki;
    init->results = results;
    init->score = 0;
    init->gold = true;
    if (init->stacki.size() ==1 && init->bufferi.size() == 1) { assert(!"bad0"); }

    HBNode* start = new HBNode(); //newhbs++; cerr << newhbs << " made\n";
    start->ps = init;
    start->parse_lstm = parse_lstm;
    // fill in the choices for start

    vector<HBNode*> active_nodes; // nodes that we have processed already and can still change our minds about
    vector<HBNode*> completed_nodes; // nodes that are final nodes for the sentence

    vector<double> best_scores; // maintain the best score at each point
    best_scores.push_back(0);
    bool first_iter = true;

    vector<Expression> log_probs;
    string rootword;
    HBNode* ans;
    bool found_correct = false;
    // while we are still searching:
    while (completed_nodes.size() < max_searches){
        // while we have not reached a finish-state:
        HBNode* cur = start;
        double base_score_penalty = start->confidence;
        while (!(cur->ps->stack.size() == 2 && cur->ps->buffer.size() == 1)) {
            // if the current node's action-choices haven't been calculated, do so
            if (!cur->initialized_choices) {
                // get list of possible actions for the current parser state
                vector<unsigned> current_valid_actions;
                for (auto a: possible_actions) {
                    if (IsActionForbidden(setOfActions[a], cur->ps->buffer.size(), cur->ps->stack.size(), cur->ps->stacki))
                        continue;
                    current_valid_actions.push_back(a);
                }
                dg.decisions_made++;
                // p_t = pbias + S * slstm + B * blstm + A * almst
                Expression p_t = affine_transform({pbias, S, cur->ps->stack_lstm.back(), B, cur->ps->buffer_lstm.back(), A, cur->ps->action_lstm.back()});
                Expression nlp_t = rectify(p_t);
                // r_t = abias + p2a * nlp
                Expression r_t = affine_transform({abias, p2a, nlp_t});
                // adist = log_softmax(r_t, current_valid_actions)
                Expression adiste = log_softmax(r_t, current_valid_actions);
                vector<float> adist = as_vector(hg->incremental_forward());
                for (unsigned i = 0; i < current_valid_actions.size(); ++i) {
                    Action action;
                    action.score = adist[current_valid_actions[i]];
                    action.val = current_valid_actions[i];
                    cur->choices.push_back(action);
                }

                sort(cur->choices.begin(), cur->choices.end(), ActionCompare());
                active_nodes.push_back(cur); //cerr << active_nodes.size()+completed_nodes.size() << "accounted for\n";
                cur->initialized_choices = true;
            }
            // select the next action to try
            Action next_action = cur->choices[cur->next_choice_i];
            cur->next_choice_i++;
            if (cur->next_choice_i < cur->choices.size())
                cur->confidence = best_scores[cur->history.size()] - (cur->ps->score + cur->choices[cur->next_choice_i].score);
            else
                cur->confidence = 999999999;

            // create a new node, copying this one's parser state
            HBNode* nhb = new HBNode(); //newhbs++; cerr << newhbs << " made\n";
            nhb->ps = new ParserState(); //newps++;
            *(nhb->ps) = *(cur->ps);
            nhb->parse_lstm = cur->parse_lstm;
            nhb->history = cur->history;
            nhb->history.push_back(cur);

            // execute the choice on the new node
            apply_action_to_state(hg, nhb->ps, next_action.val,
                                  setOfActions, sent, intToWords,
                                  cbias, H, D, R, &rootword);
            nhb->ps->score += next_action.score;

            // recalculate best-of score and confidence scores
            if (first_iter) best_scores.push_back(nhb->ps->score);
            else if (nhb->ps->score > best_scores[nhb->history.size()]) {
                best_scores[nhb->history.size()] = nhb->ps->score;
                for (HBNode* an : active_nodes) {
                    if (an->history.size() == nhb->history.size()) {
                        if (an->next_choice_i < an->choices.size())
                            an->confidence = best_scores[nhb->history.size()] - (an->ps->score + an->choices[an->next_choice_i].score);
                    }
                }
            }

            // Jacob
            if (HB_CUTOFF) {
                vector<Expression> parseembedding = {nhb->ps->stack_lstm.back(), nhb->ps->buffer_lstm.back(),
                                                     nhb->ps->action_lstm.back()};
                Expression pemb = concatenate(parseembedding);
                Expression cutoff_pemb = nobackprop(pemb);
                nhb->parse_lstm.add_input(cutoff_pemb);
            }
            // move to the newly-created node
            cur = nhb;
        }
        // this is a potential answer
        ans = cur;
        completed_nodes.push_back(ans); 

        bool error_is_predicted = true;
        Expression adiste;
        // Jacob
        if (HB_CUTOFF || build_training_graph) {
            dg.m2_decisions_made++;
            Expression p_p = affine_transform({hbbias, P, ans->parse_lstm.back()});
            adiste = log_softmax(p_p, {0, 1}); // binary_log_loss
            vector<float> scores = as_vector(hg->incremental_forward());
            error_is_predicted = scores[1] > scores[0];
        } else {
            error_is_predicted = true;
        }

        // evaluate the finished parse
        if (build_training_graph) { // learn stuff
            assert(correct_actions.size() == cur->ps->results.size());
            assert(correct_actions.size() == cur->history.size());
            (*tries)++;

            // choose next start state
            unsigned correct = 1;
            for (unsigned i=0; i < correct_actions.size(); i++) {
                if (cur->ps->results[i] != correct_actions[i]) {
                    correct = 0;
                    start = cur->history[i];
                    break;
                }
            }

            if (error_is_predicted) pef.guesses_correct++;
            else pef.guesses_incorrect++;

            // update model
            if (correct) {
                if (error_is_predicted) (*right)++;
                else pef.wrong_guesses_incorrect++;
                log_probs.push_back(pick(adiste, correct));
                found_correct = true;
                break;
            } else {
                if ( !error_is_predicted ) (*right)++;
                else pef.wrong_guesses_correct++;
                log_probs.push_back(pick(adiste, correct));
            }

        }
        else { // evaluate
            if ( !error_is_predicted ) {
                // there is no error
                unsigned correct = 1;
                found_correct = true;
                break;
            } else { // there is an error
                unsigned correct = 0;
                if (HB_CUTOFF) {
                    log_probs.push_back(pick(adiste, correct));
                }

                // choose the error node (the starting point of the next search)
                sort(active_nodes.begin(), active_nodes.end(), HBNodePointerCompareError());
                start = active_nodes.back();
                first_iter = false;
                if (start->confidence == 999999999) break; // we are out of things to search
            }
        }
    }
    sort(completed_nodes.begin(), completed_nodes.end(), HBNodePointerCompareScore());
    ans = completed_nodes.front();

    // sort completed parses and take the best answer
    results = ans->ps->results;
    stack_lstm = ans->ps->stack_lstm;
    buffer_lstm = ans->ps->buffer_lstm;
    action_lstm = ans->ps->action_lstm;
    stack = ans->ps->stack;
    stacki = ans->ps->stacki;
    buffer = ans->ps->buffer;
    bufferi = ans->ps->bufferi;

    // cleanup
    for (HBNode* node : active_nodes) {
        delete node->ps; //newps--;
        node->history.clear();
        delete node; //newhbs--;
    }
    for (HBNode* node : completed_nodes) {
        delete node->ps; //newps--;
        node->history.clear();
        delete node; //newhbs--;
    }
    active_nodes.clear();
    completed_nodes.clear();

    if (HB_CUTOFF) {
        Expression tot_neglogprob = -sum(log_probs);
        assert(tot_neglogprob.pg != nullptr);
    }

    dg.sentences_parsed++;

    return results;
}



};

void signal_callback_handler(int /* signum */) {
  if (requested_stop) {
    cerr << "\nReceived SIGINT again, quitting.\n";
    _exit(1);
  }
  cerr << "\nReceived SIGINT terminating optimization early...\n";
  requested_stop = true;
}

unsigned compute_correct(const map<int,int>& ref, const map<int,int>& hyp, unsigned len) {
  unsigned res = 0;
  for (unsigned i = 0; i < len; ++i) {
    auto ri = ref.find(i);
    auto hi = hyp.find(i);
    assert(ri != ref.end());
    assert(hi != hyp.end());
    if (ri->second == hi->second) ++res;
  }
  return res;
}

void output_conll(const vector<unsigned>& sentence, const vector<unsigned>& pos,
                  const vector<string>& sentenceUnkStrings, 
                  const map<unsigned, string>& intToWords, 
                  const map<unsigned, string>& intToPos, 
                  const map<int,int>& hyp, const map<int,string>& rel_hyp) {
  for (unsigned i = 0; i < (sentence.size()-1); ++i) {
    auto index = i + 1;
    assert(i < sentenceUnkStrings.size() && 
           ((sentence[i] == corpus.get_or_add_word(cpyp::Corpus::UNK) &&
             sentenceUnkStrings[i].size() > 0) ||
            (sentence[i] != corpus.get_or_add_word(cpyp::Corpus::UNK) &&
             sentenceUnkStrings[i].size() == 0 &&
             intToWords.find(sentence[i]) != intToWords.end())));
    string wit = (sentenceUnkStrings[i].size() > 0)? 
      sentenceUnkStrings[i] : intToWords.find(sentence[i])->second;
    auto pit = intToPos.find(pos[i]);
    assert(hyp.find(i) != hyp.end());
    auto hyp_head = hyp.find(i)->second + 1;
    if (hyp_head == (int)sentence.size()) hyp_head = 0;
    auto hyp_rel_it = rel_hyp.find(i);
    assert(hyp_rel_it != rel_hyp.end());
    auto hyp_rel = hyp_rel_it->second;
    size_t first_char_in_rel = hyp_rel.find('(') + 1;
    size_t last_char_in_rel = hyp_rel.rfind(')') - 1;
    hyp_rel = hyp_rel.substr(first_char_in_rel, last_char_in_rel - first_char_in_rel + 1);
    cout << index << '\t'       // 1. ID 
         << wit << '\t'         // 2. FORM
         << "_" << '\t'         // 3. LEMMA 
         << "_" << '\t'         // 4. CPOSTAG 
         << pit->second << '\t' // 5. POSTAG
         << "_" << '\t'         // 6. FEATS
         << hyp_head << '\t'    // 7. HEAD
         << hyp_rel << '\t'     // 8. DEPREL
         << "_" << '\t'         // 9. PHEAD
         << "_" << endl;        // 10. PDEPREL
  }
  cout << endl;
}

int main(int argc, char** argv) {
  cnn::Initialize(argc, argv);

  cerr << "COMMAND:"; 
  for (unsigned i = 0; i < static_cast<unsigned>(argc); ++i) cerr << ' ' << argv[i];
  cerr << endl;
  unsigned status_every_i_iterations = 100;

  po::variables_map conf;
  InitCommandLine(argc, argv, &conf);
  USE_POS = conf.count("use_pos_tags");

  USE_SPELLING=conf.count("use_spelling"); //Miguel
  corpus.USE_SPELLING=USE_SPELLING;

  LAYERS = conf["layers"].as<unsigned>();
  INPUT_DIM = conf["input_dim"].as<unsigned>();
  PRETRAINED_DIM = conf["pretrained_dim"].as<unsigned>();
  HIDDEN_DIM = conf["hidden_dim"].as<unsigned>();
  ACTION_DIM = conf["action_dim"].as<unsigned>();
  LSTM_INPUT_DIM = conf["lstm_input_dim"].as<unsigned>();
  POS_DIM = conf["pos_dim"].as<unsigned>();
  REL_DIM = conf["rel_dim"].as<unsigned>();
  HB_CUTOFF =  conf.count("hb_cutoff");
  GLOBAL_LOSS = conf.count("global_loss");
  DYNAMIC_BEAM = conf.count("dynamic_beam");
  MULTITHREAD_BEAMS = conf.count("multithreading");

  const unsigned beam_size = conf["beam_size"].as<unsigned>();
  const double selectional_margin = conf["selectional_margin"].as<double>();
  const unsigned hb_trials = conf["hb_trials"].as<unsigned>();
  const unsigned unk_strategy = conf["unk_strategy"].as<unsigned>();
  cerr << "Unknown word strategy: ";
  if (unk_strategy == 1) {
    cerr << "STOCHASTIC REPLACEMENT\n";
  } else {
    abort();
  }
  const double unk_prob = conf["unk_prob"].as<double>();
  assert(unk_prob >= 0.); assert(unk_prob <= 1.);
  ostringstream os;
  os << "parser_" << (USE_POS ? "pos" : "nopos")
     << '_' << LAYERS
     << '_' << INPUT_DIM
     << '_' << HIDDEN_DIM
     << '_' << ACTION_DIM
     << '_' << LSTM_INPUT_DIM
     << '_' << POS_DIM
     << '_' << REL_DIM
     << "-pid" << getpid() << ".params";
  int best_correct_heads = 0;
  double best_acc = 0;
  const string fname = os.str();
  cerr << "Writing parameters to file: " << fname << endl;
  bool softlinkCreated = false;
  corpus.load_correct_actions(conf["training_data"].as<string>());	
  const unsigned kUNK = corpus.get_or_add_word(cpyp::Corpus::UNK);
  kROOT_SYMBOL = corpus.get_or_add_word(ROOT_SYMBOL);
  DataGatherer dg;

  if (conf.count("words")) {
    pretrained[kUNK] = vector<float>(PRETRAINED_DIM, 0);
    cerr << "Loading from " << conf["words"].as<string>() << " with" << PRETRAINED_DIM << " dimensions\n";
    ifstream in(conf["words"].as<string>().c_str());
    string line;
    getline(in, line);
    vector<float> v(PRETRAINED_DIM, 0);
    string word;
    while (getline(in, line)) {
      istringstream lin(line);
      lin >> word;
      for (unsigned i = 0; i < PRETRAINED_DIM; ++i) lin >> v[i];
      unsigned id = corpus.get_or_add_word(word);
      pretrained[id] = v;
    }
  }

  set<unsigned> training_vocab; // words available in the training corpus
  set<unsigned> singletons;
  {  // compute the singletons in the parser's training data
    map<unsigned, unsigned> counts;
    for (auto sent : corpus.sentences)
      for (auto word : sent.second) { training_vocab.insert(word); counts[word]++; }
    for (auto wc : counts)
      if (wc.second == 1) singletons.insert(wc.first);
  }

  cerr << "Number of words: " << corpus.nwords << endl;
  VOCAB_SIZE = corpus.nwords + 1;

  cerr << "Number of UTF8 chars: " << corpus.maxChars << endl;
  if (corpus.maxChars>255) CHAR_SIZE=corpus.maxChars;

  ACTION_SIZE = corpus.nactions + 1;
  //POS_SIZE = corpus.npos + 1;
  POS_SIZE = corpus.npos + 10;
  possible_actions.resize(corpus.nactions);
  for (unsigned i = 0; i < corpus.nactions; ++i)
    possible_actions[i] = i;

  Model model;
  ParserBuilder parser(&model, pretrained);
  if (conf.count("model")) {
    ifstream in(conf["model"].as<string>().c_str());
    boost::archive::text_iarchive ia(in);
    ia >> model;
  }

  // OOV words will be replaced by UNK tokens
  corpus.load_correct_actionsDev(conf["dev_data"].as<string>());
  if (USE_SPELLING) VOCAB_SIZE = corpus.nwords + 1;
  //TRAINING
  if (conf.count("train")) {
    signal(SIGINT, signal_callback_handler);
    SimpleSGDTrainer sgd(&model);
    //MomentumSGDTrainer sgd(&model);
    sgd.eta_decay = 0.08;
    //sgd.eta_decay = 0.05;
    cerr << "Training started."<<"\n";
    vector<unsigned> order(corpus.nsentences);
    for (unsigned i = 0; i < corpus.nsentences; ++i)
      order[i] = i;
    double tot_seen = 0;
    status_every_i_iterations = min(status_every_i_iterations, corpus.nsentences);
    unsigned si = corpus.nsentences;
    cerr << "NUMBER OF TRAINING SENTENCES: " << corpus.nsentences << endl;
    unsigned trs = 0;
    double right = 0;
    double llh = 0;
    bool first = true;
    int iter = -1;
    while(!requested_stop) {
      ++iter;
      for (unsigned sii = 0; sii < status_every_i_iterations; ++sii) {
           if (si == corpus.nsentences) {
             si = 0;
             if (first) { first = false; } else { sgd.update_epoch(); }
             cerr << "**SHUFFLE\n";
             random_shuffle(order.begin(), order.end());
           }
           //order[si] = 37390; // DEBUGGAGE
           //cerr << "ID: " << order[si] << "\n";
           tot_seen += 1;
           const vector<unsigned>& sentence=corpus.sentences[order[si]];
           vector<unsigned> tsentence=sentence;
           if (unk_strategy == 1) {
             for (auto& w : tsentence)
               if (singletons.count(w) && cnn::rand01() < unk_prob) w = kUNK;
           }
	   const vector<unsigned>& sentencePos=corpus.sentencesPos[order[si]]; 
	   const vector<unsigned>& actions=corpus.correct_act_sent[order[si]];
           ComputationGraph hg;
          //cerr << order[si] << " " << actions.size() << "\n";
          if (beam_size == 0)
               parser.log_prob_parser(&hg,sentence,tsentence,sentencePos,actions,corpus.actions,corpus.intToWords,&right);
           else
              parser.log_prob_parser_beam(&hg, sentence, tsentence, sentencePos, actions, corpus.actions,
                                          corpus.intToWords, &right, beam_size, dg);
           double lp = as_scalar(hg.incremental_forward());
           if (lp < 0) {
             cerr << "Log prob < 0 on sentence " << order[si] << ": lp=" << lp << endl;
             assert(lp >= 0.0);
           }
           hg.backward();
           sgd.update(1.0);
           llh += lp;
           ++si;
           trs += actions.size();
      }
      sgd.status();
      cerr << "update #" << iter << " (epoch " << (tot_seen / corpus.nsentences) << ")\tllh: "<< llh<<" ppl: " << exp(llh / trs) << " err: " << (trs - right) / trs << endl;
      llh = trs = right = 0;

      static int logc = 0;
      ++logc;
      if ((beam_size == 0 && logc % 25 == 1) || logc % 250 == 1) { //((logc == 1) || (logc % 25 == 1 && (tot_seen / corpus.nsentences) > 1)) { // report on dev set
        unsigned dev_size = corpus.nsentencesDev;
        // dev_size = 100;
        double llh = 0;
        double trs = 0;
        double right = 0;
        double correct_heads = 0;
        double total_heads = 0;
        auto t_start = std::chrono::high_resolution_clock::now();
        for (unsigned sii = 0; sii < dev_size; ++sii) {
            const vector<unsigned>& sentence=corpus.sentencesDev[sii];
            const vector<unsigned>& sentencePos=corpus.sentencesPosDev[sii];
	        const vector<unsigned>& actions=corpus.correct_act_sentDev[sii];
            vector<unsigned> tsentence=sentence;
	        if (!USE_SPELLING) {
                for (auto& w : tsentence)
                    if (training_vocab.count(w) == 0) w = kUNK;
            }

            ComputationGraph hg;
            vector<unsigned> pred;
            if (beam_size == 0)
                pred = parser.log_prob_parser(&hg,sentence,tsentence,sentencePos,vector<unsigned>(),corpus.actions,corpus.intToWords,&right);
            else
                pred = parser.log_prob_parser_beam(&hg, sentence, tsentence, sentencePos, vector<unsigned>(), corpus.actions,
                                            corpus.intToWords, &right, beam_size, dg);
           double lp = 0;
           llh -= lp;
           trs += actions.size();
           map<int,int> ref = parser.compute_heads(sentence.size(), actions, corpus.actions);
           map<int,int> hyp = parser.compute_heads(sentence.size(), pred, corpus.actions);
           //output_conll(sentence, corpus.intToWords, ref, hyp);
           correct_heads += compute_correct(ref, hyp, sentence.size() - 1);
           total_heads += sentence.size() - 1;
           if (sii % 200 == 0) cerr << sii << " / " << dev_size << "\n";
        }
        auto t_end = std::chrono::high_resolution_clock::now();
        cerr << "  **dev (iter=" << iter << " epoch=" << (tot_seen / corpus.nsentences) << ")\tllh=" << llh << " ppl: " << exp(llh / trs) << " err: " << (trs - right) / trs << " uas: " << (correct_heads / total_heads) << "\t[" << dev_size << " sents in " << std::chrono::duration<double, std::milli>(t_end-t_start).count() << " ms]" << endl;
        cerr << "   Memory Usage: " << getMemoryUsage() << "\n\n";
          if (correct_heads > best_correct_heads) {
          best_correct_heads = correct_heads;
          ofstream out(fname);
          boost::archive::text_oarchive oa(out);
          oa << model;
          // Create a soft link to the most recent model in order to make it
          // easier to refer to it in a shell script.
          if (!softlinkCreated) {
            string softlink = " latest_model";
            if (system((string("rm -f ") + softlink).c_str()) == 0 && 
                system((string("ln -s ") + fname + softlink).c_str()) == 0) {
              cerr << "Created " << softlink << " as a soft link to " << fname 
                   << " for convenience." << endl;
            }
            softlinkCreated = true;
          }
        }
      }
    }
  } // should do training?



if (conf.count("train_hb")) {
//    cerr << "setup " << parser.parse_lstm.params[0][0]->dim << "\n";
    signal(SIGINT, signal_callback_handler);
    SimpleSGDTrainer sgd(&model);
    ParseErrorFeedback pef;
    //MomentumSGDTrainer sgd(&model);
    sgd.eta_decay = 0.08;
    //sgd.eta_decay = 0.05;
    cerr << "Training round 2 started."<<"\n";
    vector<unsigned> order(corpus.nsentences);
    for (unsigned i = 0; i < corpus.nsentences; ++i)
        order[i] = i;
    double tot_seen = 0;
    status_every_i_iterations = min(status_every_i_iterations, corpus.nsentences);
    unsigned si = corpus.nsentences;
    cerr << "NUMBER OF TRAINING SENTENCES: " << corpus.nsentences << endl;
    unsigned trs = 0;
    double right = 0;
    double llh = 0;
    bool first = true;
    int iter = -1;
    requested_stop = false;
    while(!requested_stop) {
        ++iter;
        pef.wrong_guesses_incorrect =0;pef.wrong_guesses_correct =0;pef.guesses_correct=0;pef.guesses_incorrect=0;
        for (unsigned sii = 0; sii < status_every_i_iterations; ++sii) {
            if (si == corpus.nsentences) {
                si = 0;
                if (first) { first = false; } else { sgd.update_epoch(); }
                cerr << "**SHUFFLE\n";
                random_shuffle(order.begin(), order.end());
            }
            tot_seen += 1;
            const vector<unsigned>& sentence=corpus.sentences[order[si]];
            vector<unsigned> tsentence=sentence;
            if (unk_strategy == 1) {
                for (auto& w : tsentence)
                    if (singletons.count(w) && cnn::rand01() < unk_prob) w = kUNK;
            }
            const vector<unsigned>& sentencePos=corpus.sentencesPos[order[si]];
            const vector<unsigned>& actions=corpus.correct_act_sent[order[si]];
            ComputationGraph hg;
            parser.log_prob_parser_hb(&hg, sentence, tsentence, sentencePos, actions, corpus.actions,
                                      corpus.intToWords, &right, &trs, 10000000, pef, dg);
            double lp = as_scalar(hg.incremental_forward());
            if (lp < 0) {
                cerr << "Log prob < 0 on sentence " << order[si] << ": lp=" << lp << endl;
                assert(lp >= 0.0);
            }
            hg.backward();
            sgd.update(1.0);
            llh += lp;
            ++si;
            //trs += actions.size();
        }
        sgd.status();
        cerr << "update #" << iter << " (epoch " << (tot_seen / corpus.nsentences) << ")\tllh: "<< llh<<" ppl: " << exp(llh / trs) << " err: " << (trs - right) / trs << endl;
        cerr << " Double Bonus: guesses_correct=" << pef.guesses_correct << " guesses_incorrect=" << pef.guesses_incorrect << " wrong_guesses_incorrect=" << pef.wrong_guesses_incorrect << " wrong_guesses_correct=" << pef.wrong_guesses_correct << "\n";
        cerr << "   Memory Usage: " << getMemoryUsage() << "\n\n";
        llh = trs = right = 0;

        static int logc = 0;
        ++logc;
        if (logc % 25 == 1) { // report on dev set
            unsigned dev_size = corpus.nsentencesDev;
            // dev_size = 100;
            double llh = 0;
            double trs = 0;
            double right = 0;
            unsigned tries = 0;
            double correct_heads = 0;
            double total_heads = 0;
            pef.wrong_guesses_incorrect =0;pef.wrong_guesses_correct =0;pef.guesses_correct=0;pef.guesses_incorrect=0;
            auto t_start = std::chrono::high_resolution_clock::now();
            for (unsigned sii = 0; sii < dev_size; ++sii) {
                const vector<unsigned>& sentence=corpus.sentencesDev[sii];
                vector<unsigned> tsentence=sentence;
                if (!USE_SPELLING) {
                    for (auto& w : tsentence)
                        if (training_vocab.count(w) == 0) w = kUNK;
                }
                const vector<unsigned>& sentencePos=corpus.sentencesPosDev[sii];
                const vector<unsigned>& actions=corpus.correct_act_sentDev[sii];

                ComputationGraph hg;
                vector<unsigned> pred;
                if (sii % 200 == 0) cerr << sii << " / " << dev_size << "\n";
                //cerr << sii << " " << actions.size() << "\n";
                if (sii == 458) continue;
                pred = parser.log_prob_parser_hb(&hg, sentence, tsentence, sentencePos, actions, corpus.actions,
                                                 corpus.intToWords, &right, &tries, 100000, pef, dg);
                double lp = 0;
                llh -= lp;
                trs += actions.size();
                map<int,int> ref = parser.compute_heads(sentence.size(), actions, corpus.actions);
                map<int,int> hyp = parser.compute_heads(sentence.size(), pred, corpus.actions);
                //output_conll(sentence, corpus.intToWords, ref, hyp);
                correct_heads += compute_correct(ref, hyp, sentence.size() - 1);
                total_heads += sentence.size() - 1;
            }
            auto t_end = std::chrono::high_resolution_clock::now();
            cerr << "  **dev (iter=" << iter << " epoch=" << (tot_seen / corpus.nsentences) << ")\tllh=" << llh << " ppl: " << exp(llh / trs) << " err: " << (trs - right) / trs << " uas: " << (correct_heads / total_heads) << "\t[" << dev_size << " sents in " << std::chrono::duration<double, std::milli>(t_end-t_start).count() << " ms]" << endl;
            cerr << " bonus stats: right=" << right << " tries=" << tries << " acc=" << right/tries << " best_acc=" << best_acc;
            cerr << " Memory Usage: " << getMemoryUsage() << "\n";
            cerr << " Double Bonus: guesses_correct=" << pef.guesses_correct << " guesses_incorrect=" << pef.guesses_incorrect << " wrong_guesses_incorrect=" << pef.wrong_guesses_incorrect << " wrong_guesses_correct=" << pef.wrong_guesses_correct << "\n\n";
            if (true || right/tries > best_acc) {
                cerr << "Saving model...\n";
                best_acc = right/tries;
                ofstream out(fname);
                boost::archive::text_oarchive oa(out);
                oa << model;
                // Create a soft link to the most recent model in order to make it
                // easier to refer to it in a shell script.
                if (!softlinkCreated) {
                    string softlink = " latest_model";
                    if (system((string("rm -f ") + softlink).c_str()) == 0 &&
                        system((string("ln -s ") + fname + softlink).c_str()) == 0) {
                        cerr << "Created " << softlink << " as a soft link to " << fname
                        << " for convenience." << endl;
                    }
                    softlinkCreated = true;
                }
            }
        }
    }
} // should do training round 2?


if (conf.count("test_hb")) {
//    cerr << "setup " << parser.parse_lstm.params[0][0]->dim << "\n";
    signal(SIGINT, signal_callback_handler);
    SimpleSGDTrainer sgd(&model);
    ParseErrorFeedback pef;
    //MomentumSGDTrainer sgd(&model);
    sgd.eta_decay = 0.08;
    //sgd.eta_decay = 0.05;
    cerr << "Testing started."<<"\n";
    vector<unsigned> order(corpus.nsentences);
    for (unsigned i = 0; i < corpus.nsentences; ++i)
        order[i] = i;
    double tot_seen = 0;
    status_every_i_iterations = min(status_every_i_iterations, corpus.nsentences);
    unsigned si = corpus.nsentences;
    cerr << "NUMBER OF TRAINING SENTENCES: " << corpus.nsentences << endl;
    unsigned trs = 0;
    double right = 0;
    double llh = 0;
    bool first = true;
    int iter = -1;
    requested_stop = false;
	llh = trs = right = 0;
	unsigned dev_size = corpus.nsentencesDev;
	// dev_size = 100;
	unsigned tries = 0;
	double correct_heads = 0;
	double total_heads = 0;
	pef.wrong_guesses_incorrect =0;pef.wrong_guesses_correct =0;pef.guesses_correct=0;pef.guesses_incorrect=0;
	auto t_start = std::chrono::high_resolution_clock::now();
	for (unsigned sii = 0; sii < dev_size; ++sii) {
		const vector<unsigned>& sentence=corpus.sentencesDev[sii];
		vector<unsigned> tsentence=sentence;
		if (!USE_SPELLING) {
			for (auto& w : tsentence)
				if (training_vocab.count(w) == 0) w = kUNK;
		}
		const vector<unsigned>& sentencePos=corpus.sentencesPosDev[sii];
		const vector<unsigned>& actions=corpus.correct_act_sentDev[sii];

		ComputationGraph hg;
		vector<unsigned> pred;
		if (sii % 200 == 0) cerr << sii << " / " << dev_size << "\n";
		//cerr << sii << " " << actions.size() << "\n";
		//if (sii == 458) continue;
		pred = parser.log_prob_parser_hb(&hg, sentence, tsentence, sentencePos, actions, corpus.actions,
										 corpus.intToWords, &right, &tries, 100000, pef, dg);
		double lp = 0;
		llh -= lp;
		trs += actions.size();
		map<int,int> ref = parser.compute_heads(sentence.size(), actions, corpus.actions);
		map<int,int> hyp = parser.compute_heads(sentence.size(), pred, corpus.actions);
		//output_conll(sentence, corpus.intToWords, ref, hyp);
		correct_heads += compute_correct(ref, hyp, sentence.size() - 1);
		total_heads += sentence.size() - 1;
	}
	auto t_end = std::chrono::high_resolution_clock::now();
	cerr << "  **dev (iter=" << iter << " epoch=" << (tot_seen / corpus.nsentences) << ")\tllh=" << llh << " ppl: " << exp(llh / trs) << " err: " << (trs - right) / trs << " uas: " << (correct_heads / total_heads) << "\t[" << dev_size << " sents in " << std::chrono::duration<double, std::milli>(t_end-t_start).count() << " ms]" << endl;
	cerr << " bonus stats: right=" << right << " tries=" << tries << " acc=" << right/tries << " best_acc=" << best_acc;
	cerr << " Memory Usage: " << getMemoryUsage() << "\n";
	cerr << " Double Bonus: guesses_correct=" << pef.guesses_correct << " guesses_incorrect=" << pef.guesses_incorrect << " wrong_guesses_incorrect=" << pef.wrong_guesses_incorrect << " wrong_guesses_correct=" << pef.wrong_guesses_correct << "\n\n";        
    
} // should do testing of cutoff?

  if (true) { // do test evaluation
    dg.decisions_made=0;dg.sentences_parsed=0;
    double llh = 0;
    double trs = 0;
    double right = 0;
    unsigned tries = 0;
    double correct_heads = 0;
    double total_heads = 0;
      ParseErrorFeedback pef;
    auto t_start = std::chrono::high_resolution_clock::now();
    unsigned corpus_size = corpus.nsentencesDev;
    for (unsigned sii = 0; sii < corpus_size; ++sii) {
      //cerr << "Starting "<< sii << "/" << corpus_size << "\n";
      const vector<unsigned>& sentence=corpus.sentencesDev[sii];
      const vector<unsigned>& sentencePos=corpus.sentencesPosDev[sii]; 
      const vector<string>& sentenceUnkStr=corpus.sentencesStrDev[sii]; 
      const vector<unsigned>& actions=corpus.correct_act_sentDev[sii];
      vector<unsigned> tsentence=sentence;
      if (!USE_SPELLING) {
        for (auto& w : tsentence)
	  if (training_vocab.count(w) == 0) w = kUNK;
      }
      ComputationGraph cg;
      double lp = 0;
      vector<unsigned> pred;
        cerr << "Completed: " << sii << " / " << corpus_size << "   Memory Usage: " << getMemoryUsage() << "\n";
      if (hb_trials > 0) {
          pred = parser.log_prob_parser_hb(&cg, sentence, tsentence, sentencePos, vector<unsigned>(), corpus.actions,
                                           corpus.intToWords, &right, &tries, hb_trials, pef, dg); }
      else if (beam_size > 0) {
          if (selectional_margin > 0) {
		cerr << "Selectional margin testing with margin " << selectional_margin << "\n";
              pred = parser.log_prob_parser_sb(&cg, sentence, tsentence, sentencePos, vector<unsigned>(),
                                                 corpus.actions, corpus.intToWords, beam_size, selectional_margin, dg);
          }
          else {
              pred = parser.log_prob_parser_beam(&cg, sentence, tsentence, sentencePos, vector<unsigned>(),
                                                 corpus.actions, corpus.intToWords, &right, beam_size, dg);
          }
      } else {
          pred = parser.log_prob_parser(&cg, sentence, tsentence, sentencePos, vector<unsigned>(), corpus.actions,
                                        corpus.intToWords, &right);
          dg.decisions_made += actions.size();
          dg.sentences_parsed++;
      }
      llh -= lp;
      trs += actions.size();
      map<int, string> rel_ref, rel_hyp;
      map<int,int> ref = parser.compute_heads(sentence.size(), actions, corpus.actions, &rel_ref);
      map<int,int> hyp = parser.compute_heads(sentence.size(), pred, corpus.actions, &rel_hyp);
      output_conll(sentence, sentencePos, sentenceUnkStr, corpus.intToWords, corpus.intToPos, hyp, rel_hyp);
      correct_heads += compute_correct(ref, hyp, sentence.size() - 1);
      total_heads += sentence.size() - 1;
    }
    auto t_end = std::chrono::high_resolution_clock::now();
      if (beam_size > 0) {
		cerr << "Beam size was " << beam_size << "\n";
	}
      if (hb_trials > 0) cerr << "HB trials was " << hb_trials << "\n";
    cerr << "TEST llh=" << llh << " ppl: " << exp(llh / trs) << " err: " << (trs - right) / trs << " dec/sent: " << dg.decisions_made/dg.sentences_parsed << " uas: " << (correct_heads / total_heads) << "\t[" << corpus_size << " sents in " << std::chrono::duration<double, std::milli>(t_end-t_start).count() << " ms]" << endl;
    cerr << "Average decisions/sentence: " << dg.decisions_made/dg.sentences_parsed << "\t[" << corpus_size << " sents in " << std::chrono::duration<double, std::milli>(t_end-t_start).count() << " ms]" << endl;
  }
  //for (unsigned i = 0; i < corpus.actions.size(); ++i) {
    //cerr << corpus.actions[i] << '\t' << parser.p_r->values[i].transpose() << endl;
    //cerr << corpus.actions[i] << '\t' << parser.p_p2a->values.col(i).transpose() << endl;
  //}
}
