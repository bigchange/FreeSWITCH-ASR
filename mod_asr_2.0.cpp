/*
 * @Author: Jerry You 
 * @CreatedDate: 2018-12-21 10:20:54 
 * @Last Modified by: Jerry You
 * @Last Modified time: 2018-12-21 17:30:03
 */

#include <switch.h>

#if defined(_WIN32)
#include <windows.h>
#include "pthread.h"
#else
#include <unistd.h>
#include <pthread.h>
#endif

#include <ctime>
#include <map>
#include <string>
#include <iostream>
#include <vector>
#include <fstream>
#include "nlsClient.h"
#include "nlsEvent.h"
#include "speechRecognizerRequest.h"

#include "nlsCommonSdk/Token.h"

#define FRAME_SIZE 3200
#define SAMPLE_RATE 16000

using std::cout;
using std::endl;
using std::ifstream;
using std::ios;
using std::map;
using std::string;
using std::vector;

using namespace AlibabaNlsCommon;

using AlibabaNls::LogDebug;
using AlibabaNls::LogInfo;
using AlibabaNls::NlsClient;
using AlibabaNls::NlsEvent;
using AlibabaNls::SpeechRecognizerCallback;
using AlibabaNls::SpeechRecognizerRequest;

SWITCH_MODULE_LOAD_FUNCTION(mod_asr_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_asr_shutdown);

extern "C" {
SWITCH_MODULE_DEFINITION(mod_asr, mod_asr_load, mod_asr_shutdown, NULL);
};

SpeechRecognizerCallback* callback;

/**
 * 全局维护一个服务鉴权token和其对应的有效期时间戳，
 * 每次调用服务之前，首先判断token是否已经过期，
 * 如果已经过期，则根据AccessKey ID和AccessKey
 * Secret重新生成一个token，并更新这个全局的token和其有效期时间戳。
 *
 * 注意：不要每次调用服务之前都重新生成新token，只需在token即将过期时重新生成即可。所有的服务并发可共用一个token。
 */
long g_expireTime = -1;

// 自定义线程参数
struct ParamStruct {
  string fileName;
  string appkey;
  string token;
};

// 自定义事件回调参数
struct ParamCallBack {
  int iExg;
  string sExg;

  pthread_mutex_t mtx;
  bool bSend;
};

typedef struct {
  switch_core_session_t* session;
  switch_media_bug_t* bug;

  SpeechRecognizerRequest* request;

  char* appKey;
  char* id;
  char* seceret;
  // char* token = NULL;
  std::string token;
  long g_expireTime = -1;
  int sampleRate;
  int stop;

} switch_da_t;

/**
 * 根据AccessKey ID和AccessKey Secret重新生成一个token，并获取其有效期时间戳
 */
int generateToken(std::string akId, std::string akSecret, std::string* token,
                  long* expireTime) {
  NlsToken nlsTokenRequest;
  nlsTokenRequest.setAccessKeyId(akId);
  nlsTokenRequest.setKeySecret(akSecret);
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                    "generate new token func [%s] [%s] --- \n", akId.c_str(), akSecret.c_str());
  int code = nlsTokenRequest.applyNlsToken();
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                    "generate new token func code [%d]\n", code);
  if (-1 == code) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                      "generate new token func failed  \n");
    return -1;
  }
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                    "generate new token value start  \n");
  *token = nlsTokenRequest.getToken();
  *expireTime = nlsTokenRequest.getExpireTime();
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                    "generate new token value finished \n");
  return 0;
}

/**
    * @brief 获取sendAudio发送延时时间
    * @param dataSize 待发送数据大小
    * @param sampleRate 采样率 16k/8K
    * @param compressRate 数据压缩率，例如压缩比为10:1的16k
   opus编码，此时为10；非压缩数据则为1
    * @return 返回sendAudio之后需要sleep的时间
    * @note 对于8k pcm 编码数据, 16位采样，建议每发送1600字节 sleep 100 ms.
            对于16k pcm 编码数据, 16位采样，建议每发送3200字节 sleep 100 ms.
            对于其它编码格式的数据, 用户根据压缩比, 自行估算, 比如压缩比为10:1的
   16k opus, 需要每发送3200/10=320 sleep 100ms.
*/
unsigned int getSendAudioSleepTime(const int dataSize, const int sampleRate,
                                   const int compressRate) {
  // 仅支持16位采样
  const int sampleBytes = 16;
  // 仅支持单通道
  const int soundChannel = 1;

  // 当前采样率，采样位数下每秒采样数据的大小
  int bytes = (sampleRate * sampleBytes * soundChannel) / 8;

  // 当前采样率，采样位数下每毫秒采样数据的大小
  int bytesMs = bytes / 1000;

  // 待发送数据大小除以每毫秒采样数据大小，以获取sleep时间
  int sleepMs = (dataSize * compressRate) / bytesMs;

  return sleepMs;
}

/**
 * @brief 调用start(), 成功与云端建立连接, sdk内部线程上报started事件
 * @note 不允许在回调函数内部调用stop(), releaseRecognizerRequest()对象操作,
 * 否则会异常
 * @param cbEvent 回调事件结构, 详见nlsEvent.h
 * @param cbParam 回调自定义参数，默认为NULL, 可以根据需求自定义参数
 * @return
 */
void OnRecognitionStarted(NlsEvent* cbEvent, void* cbParam) {
  switch_event_t* event = NULL;
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                    "OnRecognitionStarted\n");
  ParamCallBack* tmpParam = (ParamCallBack*)cbParam;
  cout << "CbParam: " << tmpParam->iExg << " " << tmpParam->sExg
       << endl;  // 仅表示自定义参数示例

  cout
      << "OnRecognitionStarted: "
      << "status code: "
      << cbEvent
             ->getStausCode()  // 获取消息的状态码，成功为0或者20000000，失败时对应失败的错误码
      << ", task id: "
      << cbEvent->getTaskId()  // 当前任务的task id，方便定位问题，建议输出
      << endl;
  // cout << "OnRecognitionStarted: All response:" << cbEvent->getAllResponse()
  // << endl; // 获取服务端返回的全部信息
  if (switch_event_create(&event, SWITCH_EVENT_CUSTOM) ==
      SWITCH_STATUS_SUCCESS) {
    event->subclass_name = strdup("asr");
    switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Event-Subclass",
                                   event->subclass_name);
    switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "ASR-Response",
                                   "OnRecognitionStarted");
    switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Channel",
                                   cbEvent->getTaskId());
    switch_event_fire(&event);
  }
}

/**
 * @brief 设置允许返回中间结果参数, sdk在接收到云端返回到中间结果时,
 * sdk内部线程上报ResultChanged事件
 * @note 不允许在回调函数内部调用stop(), releaseRecognizerRequest()对象操作,
 * 否则会异常
 * @param cbEvent 回调事件结构, 详见nlsEvent.h
 * @param cbParam 回调自定义参数，默认为NULL, 可以根据需求自定义参数
 * @return
 */
void OnRecognitionResultChanged(NlsEvent* cbEvent, void* cbParam) {
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                    "OnRecognitionResultChanged\n");
  ParamCallBack* tmpParam = (ParamCallBack*)cbParam;
  cout << "CbParam: " << tmpParam->iExg << " " << tmpParam->sExg
       << endl;  // 仅表示自定义参数示例

  cout
      << "OnRecognitionResultChanged: "
      << "status code: "
      << cbEvent
             ->getStausCode()  // 获取消息的状态码，成功为0或者20000000，失败时对应失败的错误码
      << ", task id: "
      << cbEvent->getTaskId()  // 当前任务的task id，方便定位问题，建议输出
      << ", result: " << cbEvent->getResult()  // 获取中间识别结果
      << endl;
  // cout << "OnRecognitionResultChanged: All response:" <<
  // cbEvent->getAllResponse() << endl; // 获取服务端返回的全部信息
}
/**
 * @brief sdk在接收到云端返回识别结束消息时, sdk内部线程上报Completed事件
 * @note 上报Completed事件之后, SDK内部会关闭识别连接通道.
 * 此时调用sendAudio会返回-1, 请停止发送. 不允许在回调函数内部调用stop(),
 * releaseRecognizerRequest()对象操作, 否则会异常.
 * @param cbEvent 回调事件结构, 详见nlsEvent.h
 * @param cbParam 回调自定义参数，默认为NULL, 可以根据需求自定义参数
 * @return
 */
void OnRecognitionCompleted(NlsEvent* cbEvent, void* cbParam) {
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                    "OnRecognitionCompleted\n");
  switch_event_t* event = NULL;
  ParamCallBack* tmpParam = (ParamCallBack*)cbParam;
  cout << "CbParam: " << tmpParam->iExg << " " << tmpParam->sExg
       << endl;  // 仅表示自定义参数示例

  cout
      << "OnRecognitionCompleted: "
      << "status code: "
      << cbEvent
             ->getStausCode()  // 获取消息的状态码，成功为0或者20000000，失败时对应失败的错误码
      << ", task id: "
      << cbEvent->getTaskId()  // 当前任务的task id，方便定位问题，建议输出
      << ", result: " << cbEvent->getResult()  // 获取中间识别结果
      << endl;
  // switch_event_t* event = NULL;
  if (switch_event_create(&event, SWITCH_EVENT_CUSTOM) ==
      SWITCH_STATUS_SUCCESS) {
    event->subclass_name = strdup("asr");
    switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Event-Subclass",
                                   event->subclass_name);
    switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "ASR-Response",
                                   cbEvent->getResult());
    switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Channel",
                                   cbEvent->getTaskId());
    switch_event_fire(&event);
  }
  // cout << "OnRecognitionCompleted: All response:" <<
  // cbEvent->getAllResponse() << endl; // 获取服务端返回的全部信息
}
/**
 * @brief 识别过程(包含start(), send(), stop())发生异常时,
 * sdk内部线程上报TaskFailed事件
 * @note 上报TaskFailed事件之后, SDK内部会关闭识别连接通道.
 * 此时调用sendAudio会返回-1, 请停止发送. 不允许在回调函数内部调用stop(),
 * releaseRecognizerRequest()对象操作, 否则会异常
 * @param cbEvent 回调事件结构, 详见nlsEvent.h
 * @param cbParam 回调自定义参数，默认为NULL, 可以根据需求自定义参数
 * @return
 */
void OnRecognitionTaskFailed(NlsEvent* cbEvent, void* cbParam) {
  ParamCallBack* tmpParam = (ParamCallBack*)cbParam;
  cout << "CbParam: " << tmpParam->iExg << " " << tmpParam->sExg
       << endl;  // 仅表示自定义参数示例

  cout
      << "OnRecognitionTaskFailed: "
      << "status code: "
      << cbEvent
             ->getStausCode()  // 获取消息的状态码，成功为0或者20000000，失败时对应失败的错误码
      << ", task id: "
      << cbEvent->getTaskId()  // 当前任务的task id，方便定位问题，建议输出
      << ", error message: " << cbEvent->getErrorMessage() << endl;
  // cout << "OnRecognitionTaskFailed: All response:" <<
  // cbEvent->getAllResponse() << endl; // 获取服务端返回的全部信息

  /*设置发送状态位, 停止数据发送. */
  pthread_mutex_lock(&(tmpParam->mtx));
  tmpParam->bSend = false;
  pthread_mutex_unlock(&(tmpParam->mtx));
}

/**
 * @brief 识别结束或发生异常时，会关闭连接通道,
 * sdk内部线程上报ChannelCloseed事件
 * @note 不允许在回调函数内部调用stop(), releaseRecognizerRequest()对象操作,
 * 否则会异常
 * @param cbEvent 回调事件结构, 详见nlsEvent.h
 * @param cbParam 回调自定义参数，默认为NULL, 可以根据需求自定义参数
 * @return
 */
void OnRecognitionChannelCloseed(NlsEvent* cbEvent, void* cbParam) {
  ParamCallBack* tmpParam = (ParamCallBack*)cbParam;
  cout << "CbParam: " << tmpParam->iExg << " " << tmpParam->sExg
       << endl;  // 仅表示自定义参数示例

  cout << "OnRecognitionChannelCloseed: All response:"
       << cbEvent->getAllResponse() << endl;  // 获取服务端返回的全部信息
}

static switch_bool_t asr_callback(switch_media_bug_t* bug, void* user_data,
                                  switch_abc_type_t type) {
  switch_da_t* pvt = (switch_da_t*)user_data;
  switch_channel_t* channel = switch_core_session_get_channel(pvt->session);

  switch (type) {
    case SWITCH_ABC_TYPE_INIT: {
#ifdef _WIN32
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                        "ASR Start Not Support Win32\n");
      callback = new SpeechRecognizerCallback();
#else
      callback = new SpeechRecognizerCallback();
      pvt->request = NlsClient::getInstance()->createRecognizerRequest(callback);
      if (pvt->request == NULL) {
        cout << "createRecognizerRequest failed." << endl;
      }
#endif

      if (pvt->request) {
        pvt->request->setAppKey(
            pvt->appKey);  // 设置AppKey, 必填参数, 请参照官网申请
        pvt->request->setFormat("wav");  // 设置音频数据编码格式, 可选参数,
                                   // 目前支持pcm, opu, opus, speex. 默认是pcm
        pvt->request->setSampleRate(8000);  // 设置音频数据采样率, 可选参数, 目前支持16000,
                               // 8000. 默认是16000
        pvt->request->setIntermediateResult(
            false);  // 设置是否返回中间识别结果, 可选参数. 默认false
        pvt->request->setPunctuationPrediction(
            false);  // 设置是否在后处理中添加标点, 可选参数. 默认false
        pvt->request->setInverseTextNormalization(
            false);  // 设置是否在后处理中执行ITN, 可选参数. 默认false
        std::time_t curTime = std::time(0);
        if (g_expireTime - curTime < 10) {
          switch_log_printf(
              SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
              "the token will be expired, please generate new token\n");
          
          // string tokenStr(pvt->token);
          switch_log_printf(
              SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,"generating new token\n");
          std::string idStr(pvt->id);
          std::string seceretStr(pvt->seceret);
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                            "param [%s] [%s] \n", pvt->id,
                            pvt->seceret);
          NlsToken nlsTokenRequest;
          nlsTokenRequest.setAccessKeyId(idStr);
          nlsTokenRequest.setKeySecret(seceretStr);
          int code = nlsTokenRequest.applyNlsToken();
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                            "generate new token func code [%d]\n", code);
          if (-1 == code) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                              "generate new token func failed  \n");
          }
          const char*  token = nlsTokenRequest.getToken();
          pvt->token = token;
          g_expireTime = nlsTokenRequest.getExpireTime();
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                            "generate new token [%s] [%d] \n", token,
                            g_expireTime);
        }
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                          "g_expireTime [%d] \n", g_expireTime);

        pvt->request->setToken(
            pvt->token.c_str());  // 设置账号校验token, 必填参数

        if (pvt->request->start() < 0) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                            "ASR Start Failed channel:%s\n",
                            switch_channel_get_name(channel));
          NlsClient::getInstance()->releaseRecognizerRequest(
              pvt->request);  // start()失败，释放request对象

          delete callback;
          callback = NULL;
          return SWITCH_FALSE;

        } else {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                            "ASR Start Succeed channel:%s\n",
                            switch_channel_get_name(channel));
        }
      }
    } break;
    case SWITCH_ABC_TYPE_CLOSE: {
      if (pvt->request) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                          "ASR Stop Succeed channel:%s\n",
                          switch_channel_get_name(channel));

        pvt->request->stop();
        NlsClient::getInstance()->releaseRecognizerRequest(pvt->request);
      }
    } break;

    case SWITCH_ABC_TYPE_READ_REPLACE: {
      switch_frame_t* frame;
      if ((frame = switch_core_media_bug_get_read_replace_frame(bug))) {
        char* frame_data = (char*)frame->data;
        int frame_len = frame->datalen;
        switch_core_media_bug_set_read_replace_frame(bug, frame);

        if (frame->channels != 1) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT,
                            "nonsupport channels:%d!\n", frame->channels);
          return SWITCH_FALSE;
        }

        if (pvt->request) {
          if (pvt->request->sendAudio(frame_data, frame_len, false) <= 0) {
            return SWITCH_FALSE;
          }
        }
      }

    } break;
    default:
      break;
  }

  return SWITCH_TRUE;
}

SWITCH_STANDARD_APP(stop_asr_session_function) {
  switch_da_t* pvt;
  switch_channel_t* channel = switch_core_session_get_channel(session);

  if ((pvt = (switch_da_t*)switch_channel_get_private(channel, "asr"))) {
    switch_channel_set_private(channel, "asr", NULL);
    switch_core_media_bug_remove(session, &pvt->bug);
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                      "%s Stop ASR\n", switch_channel_get_name(channel));
  }
}

SWITCH_STANDARD_APP(start_asr_session_function) {
  switch_channel_t* channel = switch_core_session_get_channel(session);

  switch_status_t status;
  switch_da_t* pvt;
  switch_codec_implementation_t read_impl;
  memset(&read_impl, 0, sizeof(switch_codec_implementation_t));

  char* argv[3] = {0};
  int argc;
  char* lbuf = NULL;

  if (!zstr(data)) {
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
                     "has data\n");
    if (lbuf = switch_core_session_strdup(session, data)) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
                        "has buff\n");

      argc = switch_separate_string(lbuf, ' ', argv,
                                    (sizeof(argv) / sizeof(argv[0])));

      if (argc >= 3) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
                          SWITCH_LOG_WARNING, "argc length [%d]\n", argc);
        switch_core_session_get_read_impl(session, &read_impl);
        
        if (!(pvt = (switch_da_t*)switch_core_session_alloc(
                  session, sizeof(switch_da_t)))) {
          switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
                            SWITCH_LOG_WARNING,
                            "switch_core_session_alloc failed!!\n");
          return;
        }
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
                          SWITCH_LOG_WARNING, "receive param started!!\n");
        pvt->stop = 0;
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
                          SWITCH_LOG_WARNING, "receive param 0!!\n");
        pvt->session = session;
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
                          SWITCH_LOG_WARNING, "receive param 1!!\n");
        // APPKEY
        char* appkey = argv[0];
        pvt->appKey = appkey;
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
                          SWITCH_LOG_WARNING, "receive param 2!!\n");
        pvt->id = argv[1];
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
                          SWITCH_LOG_WARNING, "receive param 3!!\n");
        pvt->seceret = argv[2];
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
                          SWITCH_LOG_WARNING, "receive param 4!!\n");
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
                          SWITCH_LOG_WARNING,
                          "receive param finished!!\n");
        NlsToken nlsTokenRequest;
        std::string idStr(pvt->id);
        std::string seceretStr(pvt->seceret);
        nlsTokenRequest.setAccessKeyId(idStr);
        nlsTokenRequest.setKeySecret(seceretStr);
        int code = nlsTokenRequest.applyNlsToken();
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                          "first generate new token func code [%d]\n", code);
        if (-1 == code) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                            " first generate new token func failed  \n");
        }
        const char* token = nlsTokenRequest.getToken();
        pvt->token = token;
        g_expireTime = nlsTokenRequest.getExpireTime();

        if ((status = switch_core_media_bug_add(
                 session, "asr", NULL, asr_callback, pvt, 0,
                 SMBF_READ_REPLACE | SMBF_NO_PAUSE | SMBF_ONE_ONLY,
                 &(pvt->bug))) != SWITCH_STATUS_SUCCESS) {
          switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
                            SWITCH_LOG_DEBUG, "%s Start ASR Failed!\n",
                            switch_channel_get_name(channel));
          return;
        }

        switch_channel_set_private(channel, "asr", pvt);
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                          "%s Start ASR\n", switch_channel_get_name(channel));
      }      
    }
  } else {
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
                      "%s appkey, id or secret can not be empty\n",
                      switch_channel_get_name(channel));
  }
}

SWITCH_MODULE_LOAD_FUNCTION(mod_asr_load) {
  switch_application_interface_t* app_interface;

  *module_interface =
      switch_loadable_module_create_module_interface(pool, modname);

  SWITCH_ADD_APP(app_interface, "start_asr", "asr", "asr",
                 start_asr_session_function, "", SAF_MEDIA_TAP);
  SWITCH_ADD_APP(app_interface, "stop_asr", "asr", "asr",
                 stop_asr_session_function, "", SAF_NONE);

  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, " asr_load\n");

  ParamCallBack cbParam;
  cbParam.iExg = 1;
  cbParam.sExg = "exg.";
  
  callback->setOnRecognitionStarted(OnRecognitionStarted,
                                    &cbParam);  // 设置start()成功回调函数
  callback->setOnTaskFailed(OnRecognitionTaskFailed,
                            &cbParam);  // 设置异常识别回调函数
  callback->setOnChannelClosed(OnRecognitionChannelCloseed,
                               &cbParam);  // 设置识别通道关闭回调函数
  callback->setOnRecognitionResultChanged(OnRecognitionResultChanged,
                                          &cbParam);  // 设置中间结果回调函数
  callback->setOnRecognitionCompleted(OnRecognitionCompleted,
                                      &cbParam);  // 设置识别结束回调函数

  return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_asr_shutdown) {
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, " asr_shutdown\n");

  return SWITCH_STATUS_SUCCESS;
}
