// 
// main.cpp
//
// Copyright (c) 2018. Peter Gusev. All rights reserved
//

// build
// g++ main.cpp ipc-shim.c cJSON.c -o ice-publisher -std=c++11 -lboost_thread -lboost_system -lnanomsg -lndn-cpp -lndn-cpp-tools -lcnl-cpp

#include <cstdlib>
#include <unistd.h>
#include <time.h>
#include <stdlib.h>
#include <execinfo.h>
#include <set>
#include <ctime>
#include <signal.h>
#include <cassert>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <ndn-cpp-tools/usersync/generalized-content.hpp>
#include <ndn-cpp/threadsafe-face.hpp>
#include <ndn-cpp/security/key-chain.hpp>
#include <ndn-cpp/delegation-set.hpp>
#include <cnl-cpp/generalized-object/generalized-object-stream-handler.hpp>

#define BOOST_BIND_NO_PLACEHOLDERS
#include <boost/asio.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread.hpp>
#include <boost/algorithm/string.hpp>

#include "ipc-shim.h"
#include "cJSON.h"

using namespace std;
using namespace ndn;
using namespace ndn::func_lib;
using namespace ndntools;
using namespace cnl_cpp;
using std::shared_ptr;
using std::make_shared;

static void
onRegisterFailed(const ptr_lib::shared_ptr<const Name>& prefix, bool* enabled);

static void
onRegisterSuccess
  (const ptr_lib::shared_ptr<const Name>& registeredPrefix,
   uint64_t registeredPrefixId, bool* result);

static void
printMetaInfoAndContent
  (const ptr_lib::shared_ptr<ContentMetaInfo>& metaInfo,
   const Blob& content, bool* enabled);

static void
onError
  (GeneralizedContent::ErrorCode errorCode, const string& message, bool* enabled);

//******************************************************************************
class AnnotationArray {
public: 
  AnnotationArray(string jsonString):jsonString_(jsonString){}
  AnnotationArray(const Blob& b):
    jsonString_(b.toRawStr()) {}
  AnnotationArray(const AnnotationArray& aa) : jsonString_(aa.jsonString_){}

  ~AnnotationArray(){}

  string get() const { return jsonString_; }

  friend ostream& operator << (ostream &s, const AnnotationArray& a)
  { return s << a.get(); }

private:
  string jsonString_;
};

typedef func_lib::function<void(unsigned int frameNo, const AnnotationArray&)> OnAnnotation;
typedef func_lib::function<void(unsigned int frameNo, GeneralizedContent::ErrorCode errorCode, const string& message)> OnFetchFailure;

class AnnotationConsumer {
public:
  typedef struct _FetcherListEntry {
    OnAnnotation onAnnotation_;
    OnFetchFailure onFetchFailure_;
    unsigned int frameNo_;
    unsigned int retriesLeft_;
  } FetcherListEntry;

  AnnotationConsumer(const Name& servicePrefix, std::string instance, Face* f):
    basePrefix_(servicePrefix), instance_(instance), face_(f){}
  ~AnnotationConsumer(){  }

  void fetch(unsigned int frameNo, OnAnnotation onAnnotation, OnFetchFailure onFetchFailure)
  {
    Name prefix(basePrefix_);
    prefix.appendSequenceNumber(frameNo).append(instance_);

    try {
      // if it's a new fetch - set 3 retry attempts
      if (fetchers_.find(prefix) == fetchers_.end())
        fetchers_[prefix] = {onAnnotation, onFetchFailure, frameNo, 3};
      else
      { // if it's a repeated attempt to fetch - just update 
        // callbacks, leave retry counter unchanged
        fetchers_[prefix].onAnnotation_ = onAnnotation;
        fetchers_[prefix].onFetchFailure_  = onFetchFailure;
      }

      cout << " -  spawned fetching for " << prefix << ", total " << fetchers_.size() << std::endl;

      GeneralizedContent::fetch
      (*face_, prefix, 0, 
       bind(&AnnotationConsumer::onComplete, this, _1, _2, prefix),
       bind(&AnnotationConsumer::onError, this, _1, _2, prefix));
    } 
    catch (std::exception& e) {
      cout << "exception: " << e.what() << endl;
    }
  }

private:
  Name basePrefix_;
  std::string instance_;
  Face *face_;
  map<Name, FetcherListEntry> fetchers_;

  void onComplete(const ptr_lib::shared_ptr<ContentMetaInfo>& metaInfo,
    const Blob& content, const Name objectName)
  {
    map<Name, FetcherListEntry>::iterator it = fetchers_.find(objectName);
    if (it != fetchers_.end())
    {
      FetcherListEntry e = it->second;
      if (metaInfo->getHasSegments())
        e.onAnnotation_(e.frameNo_, AnnotationArray(content));
      else
        e.onAnnotation_(e.frameNo_, AnnotationArray(metaInfo->getOther()));
      fetchers_.erase(it);

      cout << "  * received " << objectName 
      << ", content-type: " << metaInfo->getContentType()
      << " (has segments: " << (metaInfo->getHasSegments() ? "YES)" : "NO)")
      << " size: " << (metaInfo->getHasSegments() ? content.size() : metaInfo->getOther().toRawStr().size())
      << ", remaining " << fetchers_.size()
      << endl;
    }
    else
      throw std::runtime_error("fetcher entry not found");
  }

  void onError(GeneralizedContent::ErrorCode errorCode, const string& message, 
    const Name objectName)
  {
    cout << "error fetching " << objectName << ": " << message << endl;

    map<Name, FetcherListEntry>::iterator it = fetchers_.find(objectName);
    if (it != fetchers_.end())
    {
      FetcherListEntry& e = fetchers_[objectName];
      if (e.retriesLeft_ > 0)
      {
        e.retriesLeft_--;
        fetch(e.frameNo_, e.onAnnotation_, e.onFetchFailure_);
      }
      else
      {
        e.onFetchFailure_(e.frameNo_, errorCode, message);
        fetchers_.erase(it);
      }
    }
    else
      throw std::runtime_error("fetcher entry not found");
  }
};

class AnnotationPublisher {
public:
  /**
   * @param userPrefix According to namespace design, smth. like "/icear/user/<user-id>"
   * @param serviceName According to namespace design, smth. like "object_recognizer"
   * @param cache Memory content cache for storing published data
   * @param keyChain Key chain for signing published packets
   * @param certificateName Certificate name to use for signing
   */
  AnnotationPublisher(const Name& servicePrefix, Face* face, KeyChain *keyChain)
    : baseName_(servicePrefix)
    , face_(face)
    , keyChain_(keyChain) {}

  ~AnnotationPublisher() {}

  void publish(unsigned int frameNo, const AnnotationArray& a, const string& engine){
    string content = a.get();

    getHandlerForEngine(engine)->setObject(frameNo, Blob::fromRawStr(content), "application/json");

    std::cout << "*   published annotation for " << frameNo << " under " 
              << getHandlerForEngine(engine)->getNamespace().getName() << std::endl;
  }

private:
  map<string, std::shared_ptr<Namespace>> namespaces_;
  map<string, std::shared_ptr<GeneralizedObjectStreamHandler> > handlers_;
  Face *face_;
  KeyChain *keyChain_;
  Name baseName_;

  std::shared_ptr<GeneralizedObjectStreamHandler> getHandlerForEngine(const string& engine)
  {
      if (handlers_.find(engine) == handlers_.end())
      {
          Name streamPrefix(baseName_);
          streamPrefix.append(engine);
          namespaces_[engine] = std::make_shared<Namespace>(streamPrefix, keyChain_);
          handlers_[engine] = std::make_shared<GeneralizedObjectStreamHandler>();

          namespaces_[engine]->setHandler(handlers_[engine]);
          namespaces_[engine]->setFace(face_, [](const shared_ptr<const Name>& prefix){
              cerr << "Register failed for prefix " << prefix->toUri() << endl; 
          });
      }

      return handlers_[engine];
  }

};

//******************************************************************************
void handler(int sig) {
  void *array[10];
  size_t size;

  // get void*'s for all entries on the stack
  size = backtrace(array, 10);

  // print out all the frames to stderr
  fprintf(stderr, "Error: signal %d:\n", sig);
  backtrace_symbols_fd(array, size, STDERR_FILENO);
  exit(1);
}

//******************************************************************************
static std::string pipeName = "/tmp/ice-annotations";
static int feature_pipe = -1;
static std::string dbPipeName = "/out/ice-annotations";
static int db_pipe = -1;

int create_pipe(const char* fname)
{
  int res = 0;
  do {
    res = mkfifo(fname, 0644);
    if (res < 0 && errno != EEXIST)
    {
      printf("error creating pipe (%d): %s\n", errno, strerror(errno));
      sleep(1);
    }
    else res = 0;
  } while (res < 0);

  return 0;
}

void reopen_readpipe(const char* fname, int* pipe)
{
    do {
        if (*pipe > 0) 
            close(*pipe);

        *pipe = open(fname, O_RDONLY);

        if (*pipe < 0)
            printf("> error opening pipe: %s (%d)\n",
                strerror(errno), errno);
    } while (*pipe < 0);
}

int writeExactly(uint8_t *buffer, size_t bufSize, int pipe)
{   
    int written = 0, r = 0; 
    int keepWriting = 0;

    do {
        r = write(pipe, buffer+written, bufSize-written);
        if (r > 0) written += r;
        keepWriting = (r > 0 && written != bufSize) || (r < 0 && errno == EAGAIN);
    } while (keepWriting == 1);

    return written;
}

void dumpAnnotations(const char* annotations, size_t len)
{
    // Open db pipe...
    if (db_pipe < 0)
    {
        db_pipe = ipc_setupPubSinkSocket(dbPipeName.c_str());
        if (db_pipe < 0)
        {
            printf("> failed to setup socket %s: %s (%d)\n", 
                dbPipeName.c_str(), ipc_lastError(), ipc_lastErrorCode());
            exit(1);
        }
        else
          printf("> opened db socket (%s)\n", dbPipeName.c_str());
    }

    if (db_pipe >= 0)
    {
        // remove all newlines
        // boost::replace_all(annotations, "\r\n", "");

        cout << "> dumping annotations to DB... " << std::endl;
        int res = ipc_sendData(db_pipe, (void*)(annotations), len);

        if (res < 0)
            cout << "> error dumping annotations (" << ipc_lastErrorCode() << "): " << ipc_lastError() << std::endl;
        else
            cout << "> dumped annotations to DB socket" << std::endl;
    }
}

std::string readAnnotations(int pipe, unsigned int &frameNo, std::string &engine)
{
  char *annotations;
  int len = ipc_readData(feature_pipe, (void**)&annotations);

  if (len > 0)
  {
    // pass it forward to a 1-to-M pipe
    dumpAnnotations(annotations, len);

    cJSON *item = cJSON_Parse(annotations);
    
    if (item)
    {
      cJSON *fNo = cJSON_GetObjectItem(item, "playbackNo");
      cJSON *eng = cJSON_GetObjectItem(item, "engine");
      cJSON *array = cJSON_GetObjectItem(item, "annotations");

      if (cJSON_IsArray(array) && cJSON_IsNumber(fNo))
      {
        char *annStr = cJSON_Print(array);
        engine = std::string(eng->valuestring);
        frameNo = fNo->valueint;
        string s(annStr);
        free(annStr);

        cout << "> read annotations (frame " << frameNo << ", engine " << engine << ")" << std::endl;

        return s;
      }
      else
        cout << "JSON is poorely formatted" << endl;
    }
    else
      cout << "> error parsing JSON: " << annotations << endl;
    free(annotations);
  }

  return "";
}

//******************************************************************************
int main(int argc, char** argv)
{
  signal(SIGABRT, handler);
  std::srand(std::time(0));

  // usage: test-annotations-example <basePrefix> <userId> <serviceName> <annotationsFile>
  if (argc < 6)
  {
    std::cout << "usage: " << argv[0] << " <basePrefix> <userId> <serviceName> <annotationsFile> <dbPipeFile>" << std::endl;
    exit(1);
  }

  std::string basePrefix = std::string(argv[1]);  // "/icear/user";
  std::string userId = std::string(argv[2]);      // "peter";
  std::string service = std::string(argv[3]);     // "object_recognizer";
  pipeName = std::string(argv[4]);
  dbPipeName = std::string(argv[5]);

  try {
    boost::asio::io_service io;
    shared_ptr<boost::asio::io_service::work> work(make_shared<boost::asio::io_service::work>(io));
    boost::thread t = boost::thread([&io](){ 
      try {
        io.run();
      }
      catch (std::exception &e)
      {
        std::cout << "caught exception on main thread: " << e.what() << std::endl;
      }
    });

    KeyChain keyChain;
    Name certificateName = keyChain.getDefaultCertificateName();
    // The default Face will connect using a Unix socket, or to "localhost".
    ThreadsafeFace producerFace(io);
    producerFace.setCommandSigningInfo(keyChain, certificateName);
    // MemoryContentCache contentCache(&producerFace);
    Name servicePrefix(basePrefix);

    servicePrefix.append(userId).append(service);
    
    std::cout << "> reading annotations from " << pipeName << std::endl;
    std::cout << "> passing annotations to " << dbPipeName << std::endl;
    std::cout << "> will publish under " << servicePrefix << std::endl;

    bool enabled = true;
    bool registrationResultSuccess = false;
    unsigned int frameNo;

    std::map<unsigned int, AnnotationArray> acquiredAnnotations;

    AnnotationPublisher apub(servicePrefix, &producerFace, &keyChain);

    unsigned int n = 10, npublished = 0;
    std::set<int> publishedFrames;

    // Open the feature pipe (from YOLO)
    cout << "> opening pipe..." << std::endl;
    if (feature_pipe < 0)
    {
        feature_pipe = ipc_setupSubSourceSocket(pipeName.c_str());

        if (feature_pipe < 0)
        {
            printf("> failed to setup socket %s: %s (%d)\n", 
                pipeName.c_str(), ipc_lastError(), ipc_lastErrorCode());
            exit(1);
        }
        else
          printf("> opened feature socket (%s)\n", pipeName.c_str());
    }

    while(enabled){
      
      std::string engine;
      std::string annotations = readAnnotations(feature_pipe, frameNo, engine);

      if (annotations !="")
      {
        io.dispatch([&apub, frameNo, annotations, engine](){
          apub.publish(frameNo, AnnotationArray(annotations), engine);
        });
      }
    } // while true
  } catch (std::exception& e) {
    cout << "exception: " << e.what() << endl;
  }

  return 0;
}

/**
 * This is called to print an error if the MemoryContentCache can't register
 * with the local forwarder.
 * @param prefix The prefix Name to register.
 * @param enabled On success or error, set *enabled = false.
 */
static void
onRegisterFailed(const ptr_lib::shared_ptr<const Name>& prefix, bool* enabled)
{
  *enabled = false;
  cout << "> Failed to register prefix " << prefix->toUri() << endl;
}

static void
onRegisterSuccess
  (const ptr_lib::shared_ptr<const Name>& registeredPrefix,
   uint64_t registeredPrefixId, bool* result)
{
  *result = true;
  cout << "> Successfully registered prefix " << *registeredPrefix << std::endl;
}
