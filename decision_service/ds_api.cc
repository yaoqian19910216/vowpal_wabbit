#include "ds_api.h"
#include "ds_internal.h"
#include "ds_vw.h"
#include "event_hub_client.h"

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/lockfree/queue.hpp>

#include <cpprest/http_client.h>

#include <memory>
#include <chrono>
#include <thread>

// DS config
#include <locale>
#include <codecvt>
// SAS code
using namespace std::chrono_literals;

namespace Microsoft {
  namespace DecisionService {

    using namespace std;
    using namespace utility;
    using namespace web::http;
    using namespace web::http::client;
    using namespace concurrency;
    using namespace concurrency::streams;

    DecisionServiceConfiguration DecisionServiceConfiguration::Download(const char* url)
    {
      // download configuration
      http_client client(conversions::to_string_t(url));
      auto json = client
        .request(methods::GET)
        .then([=](http_response response) { return response.extract_json(/* ignore content type */ true); })
        .get();

      DecisionServiceConfiguration config;
      config.model_url = conversions::to_utf8string(json[U("ModelBlobUri")].as_string());
      config.eventhub_interaction_connection_string = conversions::to_utf8string(json[U("EventHubInteractionConnectionString")].as_string());
      config.eventhub_observation_connection_string = conversions::to_utf8string(json[U("EventHubObservationConnectionString")].as_string());
      // with event sizes of 3kb & 5s batching, 2 connections can get 50Mbps
      config.num_parallel_connection = 2;
      config.batching_timeout_in_seconds = 5;
      config.batching_queue_max_size = 8 * 1024;
      return config;
    }


    class DecisionServiceClientInternal {
    private:
      unique_ptr<VowpalWabbitThreadSafe> _pool;
      DecisionServiceConfiguration _config;

      boost::lockfree::queue<vector<unsigned char>*> _queue;

      std::vector<EventHubClient> _event_hub_interactions;
      EventHubClient _event_hub_observation;

      atomic_bool _thread_running;

      thread _upload_interaction_thread;
      thread _download_model_thread;

    public:
      DecisionServiceClientInternal(DecisionServiceConfiguration config)
        : _pool(new VowpalWabbitThreadSafe()),
        _config(config),
        _queue(config.batching_queue_max_size),
        _thread_running(true),
        // initialize n-clients
        _event_hub_interactions(config.num_parallel_connection, EventHubClient(config.eventhub_interaction_connection_string)),
        _event_hub_observation(config.eventhub_observation_connection_string),
        _upload_interaction_thread(&DecisionServiceClientInternal::upload_interactions, this),
        _download_model_thread(&DecisionServiceClientInternal::download_model, this)
      { }

      ~DecisionServiceClientInternal()
      {
        _thread_running = false;
        _upload_interaction_thread.join();

        // delete all elements from _queue
        vector<unsigned char>* item;
        while (_queue.pop(item))
          delete item;
      }

      void upload_reward(const char* event_id, const char* reward)
      {
        ostringstream payload;
        payload << "{\"EventId\":\"" << event_id << "\",\"v\":" << reward << "}";

        _event_hub_observation.Send(payload.str().c_str());
      }

      void enqueue_interaction(RankResponse& rankResponse)
      {
        std::ostringstream ostr;
        ostr << rankResponse;
        auto s = ostr.str();

        vector<unsigned char>* buffer = new vector<unsigned char>(s.begin(), s.end());
        _queue.push(buffer);
      }

    private:
      void download_model()
      {
        // TODO: download using cpprest
      }

      void upload_interactions()
      {
        vector<unsigned char>* json;
        vector<unsigned char>* json2;

        // populate initial already finished tasks
        vector<pplx::task<http_response>> open_requests;

        http_response initial_response;
        initial_response.set_status_code(201);

        for (auto& client : _event_hub_interactions)
        {
          pplx::task_completion_event<http_response> evt;
          evt.set(initial_response);
          open_requests.push_back(pplx::task<http_response>(evt));
        }

        while (_thread_running)
        {
          if (!_queue.pop(json))
          {
            // empty
            // cout << "queue empty" << endl;
            std::this_thread::sleep_for(std::chrono::seconds(_config.batching_timeout_in_seconds));

            continue;
          }

          int batch_count = 1;

          do
          {
            if (!_queue.pop(json2))
              json2 = nullptr;
            else
            {
              // check if we enough space
              if (json->size() + json2->size() < 256 * 1024 - 1)
              {
                // move get pointer to the beginning
                json->push_back('\n');
                json->insert(json->end(), json2->begin(), json2->end());

                batch_count++;

                // free memory
                delete json2;

                // accumulate more
                continue;
              }
            }

            // manage multiple outstanding requests...
            // send data
            auto ready_response = pplx::when_any(open_requests.begin(), open_requests.end()).get();

            // need to wait delete
            //// materialize payload
            //string json_str = json->str();

            //// free memory
            //delete json;

            //printf("ready: %d length: %d batch count: %d status: %d\n",
            //  (int)ready_response.second, (int)json_str.length(), batch_count, ready_response.first.status_code());

            // send request and add back to task list
            open_requests[ready_response.second] =
              _event_hub_interactions[ready_response.second].Send(json);

            // continue draining if we have another non-append json element
            json = json2;
            batch_count = 1;
          } while (json);
        }
      }
    };

    // TODO: move to seperate file
    RankResponse::RankResponse(std::vector<int>& ranking, const char* pevent_id, const char* pmodel_version, std::vector<float>& pprobabilities, const char* pfeatures)
      : _ranking(ranking), event_id(pevent_id), model_version(pmodel_version), _probabilities(pprobabilities), features(pfeatures)
    {
      // check that ranking has more than 0 elements
    }

    int RankResponse::length()
    {
      return _ranking.size();
    }

    int RankResponse::top_action()
    {
      // we always have at least 1 element
      return _ranking[0];
    }

    int RankResponse::action(int index)
    {
      if (index < 0 || index >= _ranking.size())
        return -1; // TODO: throw argument exception

      return _ranking[index];
    }

    float RankResponse::probability(int index)
    {
      if (index < 0 || index >= _probabilities.size())
        return -1; // TODO: throw argument exception

      return _probabilities[index];
    }

    const std::vector<float>& RankResponse::probabilities() { return _probabilities; }

    const std::vector<int>& RankResponse::ranking() { return _ranking; }

    std::ostream& operator<<(std::ostream& ostr, const RankResponse& rankResponse)
    {
      // JSON format
      // {"Version":"1","EventId":"b94a280e32024acb9a4fa12b058157d3","a":[13,11,7,2,1,5,9,3,10,6,8,14,4,12]
      // ,"c":{"Geo":{"country":"United States","_countrycf":"8",...},
      // "p":[0.814285755,0.0142857144,0.0142857144,0.0142857144,0.0142857144,0.0142857144,0.0142857144,0.0142857144,0.0142857144,0.0142857144,0.0142857144,0.0142857144,0.0142857144,0.0142857144],
      // "VWState":{"m":"680ec362b798463eaf64489efaa0d7b1/d13d8ad87e6f4af3b090b0f9cb9faaac"}}

      // TODO: locales?
      ostr << "{\"Version\":\"1\",\"EventId\":\"" << rankResponse.event_id << "\","
        "\"a\":[";

      // insert ranking
      for (auto& r : rankResponse._ranking)
        ostr << r << ",";
      // remove last ,
      ostr.seekp(-1, ostr.cur);

      ostr << "],\"c\":{" << rankResponse.features << "},"
        "\"p\":[";

      // insert ranking
      for (auto& r : rankResponse._probabilities)
        ostr << r << ",";
      // remove last ,
      ostr.seekp(-1, ostr.cur);

      // include model version
      ostr << "],\"VWState\":{\"m\":\"" << rankResponse.model_version << "\"}}";
      return ostr;
    }

    DecisionServiceClient::DecisionServiceClient(DecisionServiceConfiguration& config)
      : _state(new DecisionServiceClientInternal(config))
    { }

    DecisionServiceClient::~DecisionServiceClient()
    { }

    RankResponse* DecisionServiceClient::rank(const char* context, const char* event_id, Array<int>& default_ranking)
    {
      return rank(context, event_id, default_ranking.data, default_ranking.length);
    }

    RankResponse* DecisionServiceClient::rank(const char* context, const char* event_id, int* default_ranking, int default_ranking_size)
    {
      // generate event id if empty
      std::string l_event_id;
      if (!event_id || strlen(event_id) == 0)
        l_event_id = boost::uuids::to_string(boost::uuids::random_generator()());
      else
        l_event_id = event_id;

      // TODO: invoke VW
      // TODO: if no model provided use default ranking and impose epsilon-greedy exploration

      // invoke scoring to get a distribution over actions
      // std::vector<ActionProbability> ranking = _pool->rank(context);
      std::vector<int> ranking;
      ranking.push_back(1);
      ranking.push_back(0);

      std::vector<float> probs;
      probs.push_back(0.8f);
      probs.push_back(0.2f);

      RankResponse* response = new RankResponse(
        ranking,
        l_event_id.c_str(), "m1", probs, context);

      // invoke sampling
      // need to port random generator
      // need to port top slot explortation

      _state->enqueue_interaction(*response);

      return response;
    }

    void DecisionServiceClient::reward(const char* event_id, const char* reward)
    {
      _state->upload_reward(event_id, reward);
    }

    void DecisionServiceClient::update_model(unsigned char* model, size_t offset, size_t len)
    {
      // TODO: swap out model
    }
  }

  int main(int argc, char *argv[])
  {
    std::cout << "Decision Service simulator" << std::endl;

    // TODO: allow serialization to/from JSON, speed up startup?
    Microsoft::DecisionService::DecisionServiceConfiguration config = Microsoft::DecisionService::DecisionServiceConfiguration::Download("https://storagev6d6i5yogkqpq.blob.core.windows.net/mwt-settings/client?sv=2017-04-17&sr=b&sig=ndoUJ0esholNKpH4QDV%2BB5IVhdM0zYixYctHXfobpqQ%3D&st=2017-12-27T17%3A29%3A11Z&se=2027-12-27T17%3A30%3A11Z&sp=r");

    Microsoft::DecisionService::DecisionServiceClient client(config);

    std::ostringstream payload;
    payload << "[";
    for (int i = 0; i < 1024; i++)
    {
      if (i > 0)
        payload << ",";
      payload << rand() % 100;
    }
    payload << "]";
    std::string payload_str = payload.str();

    while (true)
    {
      client.rank(payload_str.c_str(), nullptr, nullptr, 0);
      std::this_thread::sleep_for(1ms / 10);
    }

    int x;
    std::cin >> x;
    return 0;
  }
}