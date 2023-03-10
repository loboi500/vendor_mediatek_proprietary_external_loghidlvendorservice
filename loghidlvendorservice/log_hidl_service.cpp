#include <hidl/LegacySupport.h>
#include <string.h>
#include <cutils/properties.h>
#include <dlfcn.h>

#include "log_hidl_service.h"

namespace vendor {
namespace mediatek {
namespace hardware {
namespace log {
namespace V1_0 {
namespace implementation {

using ::android::wp;
using ::android::status_t;
using ::android::hardware::hidl_death_recipient;
using ::android::hidl::base::V1_0::IBase;

#ifndef DEVPATH32
#define DEVPATH32 "/vendor/lib/libdynamiclog.so"
#endif

#ifndef DEVPATH64
#define DEVPATH64 "/vendor/lib64/libdynamiclog.so"
#endif

#if INTPTR_MAX == INT32_MAX
#define DEVPATH DEVPATH32
#elif INTPTR_MAX == INT64_MAX
#define DEVPATH DEVPATH64
#else
#error "Not 32/64 bit defined"
#endif

int cpp_main() {
	LOGI("log_hidl_vendor_service_main 2018_07_201 runs");

	::android::hardware::configureRpcThreadpool(20, true);

	sp<ILog> mMdlogConnectService = new LogHidlService("ModemLogHidlServer");
	sp<ILog> mMobilelogConnectService = new LogHidlService("MobileLogHidlServer");
    sp<ILog> mATMWiFiConnectService = new LogHidlService("ATMWiFiHidlServer");
    sp<ILog> mconnFWConnectService = new LogHidlService("ConnsysFWHidlServer");
    sp<ILog> mLoggerConnectService = new LogHidlService("LoggerHidlServer");

	::android::hardware::joinRpcThreadpool();
	return 0;
}

//LbsHidlService
LogHidlService:: LogHidlService(const char* name) :
    mLogHidlDeathRecipient(new LogHidlDeathRecipient(this)) {
    int i;
    for (i = 0; i < 10; i++) {
        client[i].fd = -1;
        memset((void *) &(client[i].addr), 0,sizeof(struct sockaddr));
    }
    clientConnect = -1;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    pthread_mutex_init(&mlock, NULL);
    m_socketID = -1;
    m_threadID = 0;

	shouldtimeout = true;
	m_socketID = -1;
	mLogCallback = nullptr;
    m_thread = 0;
	strncpy(m_Name, name, sizeof(m_Name) - 1);
	m_Name[sizeof(m_Name) - 1] = '\0';
	setDynamicLog = NULL;
	dymamicLogHandle = NULL;
	init();

}

void LogHidlService::init() {
	LOGI("loghidlvendorservice name = [%s]", m_Name);
    status_t status;
    status = this->registerAsService(m_Name);

    if(status != 0) {
    	LOGI("loghidlvendorservice() registerAsService() for name=[%s] failed status=[%d]",
        		m_Name, status);
    } else {
    	LOGI("loghidlvendorservice() registerAsService() for name=[%s] successful status=[%d]",
            		m_Name, status);
    }

    LOGI("Create loghidlvendorservice Done for m_Name = %s!", m_Name);
    if (!strncmp(m_Name, "LoggerHidlServer", strlen("LoggerHidlServer"))) {
        return;
    }
    initSocketServer();
}

void LogHidlService::tryInitDynamicLog() {
    if (dymamicLogHandle != NULL || setDynamicLog != NULL) return;
    dymamicLogHandle = dlopen(DEVPATH, RTLD_LAZY);
    if (dymamicLogHandle != NULL) {
        setDynamicLog = (setDynamicLog_T)dlsym(dymamicLogHandle, "setDynamicLog");
        if (setDynamicLog == NULL) {
            LOGE("dlsym setDynamicLog error");
            return;
        }
    } else {
        LOGE("dlopen %s error", DEVPATH);
        return;
    }
    LOGD("dlopen/dlsym successed, dymamicLogHandle: %p, setDynamicLog: %p", dymamicLogHandle, setDynamicLog);
}

void LogHidlService::initSocketServer() {
    int ret;
    do {
        m_socketID = socket_local_server(m_Name,
                ANDROID_SOCKET_NAMESPACE_ABSTRACT, SOCK_STREAM);

        if (m_socketID < 0) {
            LOGE("m_socketID<0 ");
			LOGE("socket server create failed , errno = %d", errno);
            break;
        }
//        MDL_LOGV("mSockListenId ok\n");

        ret = listen(m_socketID, 4);
        if (ret < 0) {
            LOGE("listen error\n");
            break;
        }
		m_threadID = pthread_create(&m_thread, NULL, socketListener,  this);
        if(m_threadID)
	    {
		  LOGI("[%s] Failed to create socket server thread!", m_Name);
		  break;
	 	} else {
		  LOGD("[%s] Success to setup socket local server!", m_Name);
		  return;
		}

    } while (0);
    LOGD("Fail to setup socket local server, exit(2)");
    if (m_socketID > 0) {
        shutdown(m_socketID, 2);
        close(m_socketID);
    }
    exit(2);
}

void *LogHidlService::socketListener(void *p)
{
	if(p == NULL)
	{
		LOGI("[Socket] socket thread parameter error!");
		return NULL;
	}
	
	LogHidlService *pServer = (LogHidlService *)p;
	pServer->runListener();
	return NULL;
}

void LogHidlService::runListener() {
    LOGD("runListener");
    int max = 0;
    int rc = 0;
    int i = 0;
    fd_set read_fds;
    while (1) {
        FD_ZERO(&read_fds);
        max = m_socketID;
        FD_SET(m_socketID, &read_fds);

        pthread_mutex_lock(&mlock);

        for (i = 0; i < 10; i++) {
            if (client[i].fd > 0) {
                FD_SET(client[i].fd, &read_fds);
                if (client[i].fd > max) {
                    max = client[i].fd;
                }
            }
        }

        pthread_mutex_unlock(&mlock);

 //       MDL_LOGV("mdlogger socket select start");
        if ((rc = select(max + 1, &read_fds, NULL, NULL,
                shouldtimeout ? &timeout : NULL)) < 0) {
            LOGE("select failed (%s), errno = %d", strerror(errno), errno);
            sleep(1);
            continue;
        } else if (!rc) {
            LOGE("select timeout");
            shouldtimeout = false;
            continue;
        }
        LOGD("loghidlvendorservice select done");
        shouldtimeout = false;
        // deal with new connection
        if (FD_ISSET(m_socketID, &read_fds)) {
            struct sockaddr addr;
            socklen_t alen = sizeof(addr);
            int connectfd;

            LOGD("loghidlvendorservice begin to accept");

            if ((connectfd = accept(m_socketID, &addr, &alen)) < 0) {
                LOGE(
                        "accept failed (%s) , errno = %d", strerror(errno), errno);
                sleep(1);
                continue;
            }

            LOGD("loghidlvendorservice accept done");

            pthread_mutex_lock(&mlock);
            for (i = 0; i < 10; i++) {
                if (client[i].fd < 0) {
                    client[i].fd = connectfd;
                    client[i].addr = addr;
                    break;
                }
            }
            if (connectfd != clientConnect) {
                clientConnect = connectfd;
            }
            pthread_mutex_unlock(&mlock);
        }
        do {
            // someone writes the socket
            pthread_mutex_lock(&mlock);
            for (i = 0; i < 10; i++) {
                int fd = client[i].fd;
                if (fd < 0) {
                    continue;
                }
                if (FD_ISSET(fd, &read_fds)) {
                    if (!SocketServerhandleMessage(fd)) {
                        close(fd);
                        LOGE("loghidlvendorservice client:%d broken",fd);
                        FD_CLR(fd, &read_fds);
                        client[i].fd = -1;
                        memset((void *) &(client[i].addr), 0,
                                sizeof(struct sockaddr));
                    }
                    FD_CLR(fd, &read_fds);
                    continue;
                }
            }
            pthread_mutex_unlock(&mlock);
        } while (0);
    }
}
bool LogHidlService::SocketServerhandleMessage(int fd) {
	char buffer[256];
    memset(buffer, '\0', 256);

    int len;
    if ((len = read(fd, buffer, sizeof(buffer) - 1)) < 0) {
        LOGE("read() failed (%s), errno = %d", strerror(errno), errno);
        return false;
    } else if (!len) {
        return false;
    }
    buffer[sizeof(buffer) - 1] = '\0';
    //SANITY TEST LOG LABEL. DONOT CHANGE
    int retryCount = 400;
    LOGI("%s SocketServerhandleMessage receive [%s], from fd = %d", m_Name, buffer, fd);
	while(retryCount-- > 0) {
                if (mLogCallback != nullptr) {
			break;
		}
		usleep(10000);
	}
	LOGI("%s check mLogCallback is not null retryCount = %d", m_Name, retryCount);
	if (mLogCallback != nullptr) {
                LOGI("%s send hidl client [%s]", m_Name, buffer);
        buffer[sizeof(buffer) - 1] = '\0';
		mLogCallback->callbackToClient(buffer);
		return true;
	}
	if ((len + 2) < 256) {
            buffer[len] = ',';
            buffer[len + 1] = '0';
            buffer[len + 2] = '\0';
	    LOGE("%s send hidl client [%s] fail,for hal client dead!", m_Name, buffer);
		SendMessageToSocketClient(buffer);
	}
	return true;
}


//#########################old#################


LogHidlService::~LogHidlService() {
	LOGI("[%s] ~LogHidlService", m_Name);
    if (dymamicLogHandle != NULL) {
        dlclose(dymamicLogHandle);
    }
}

void LogHidlService::handleHidlDeath() {
    LOGD("handleHidlDeath():client [%s] died. ", m_Name);
    mLogCallback = nullptr;

}
Return<void> LogHidlService::setCallback(const sp<ILogCallback>& callback) {
	LOGI("[%s] setCallback()", m_Name);
    mLogCallback = callback;
	mLogCallback->linkToDeath(mLogHidlDeathRecipient, 0);
    return Void();
}
//receive sys client message, need send it to socket client.
Return<bool> LogHidlService::sendToServer(const hidl_string& data) {
    LOGI("[%s] sendToServer() data = [%s]", m_Name, data.c_str());
  //const char * msg;
  //msg = data.c_str();

  char msg[256] = { 0 };
  memset(msg, '\0', 256 * sizeof(char));
  if(strlen(data.c_str())<256) {
     strncpy(msg, data.c_str(),sizeof(msg)-1);
  }
  msg[sizeof(msg)-1] = '\0';
  if (!strncmp(msg, "hidl_log_mtk_vts_test", strlen("hidl_log_mtk_vts_test"))) {
    if (mLogCallback != nullptr) {
        mLogCallback->callbackToClient("hidl_log_mtk_vts_test,1");
    }
    LOGI("%s receive vts test string,no need work just return", m_Name);
    return true;
  }
  // "set_property,name,value"
  if (!strncmp(msg, "set_property,", strlen("set_property,"))) {
      std::string msgStr = msg;
      auto pos1 = msgStr.find("::");
      if (pos1 != std::string::npos) {
          msgStr.replace(pos1, 2, ",");
          auto pos2 = msgStr.find("::");
          if (pos2 != std::string::npos) {
              msgStr.replace(pos2, 2, ",");
              tryInitDynamicLog();
              LOGI("call setDynamicLog, setDynamicLog: %p, msg: %s", setDynamicLog, msgStr.c_str());
              return setDynamicLog != NULL ? setDynamicLog(const_cast<char *>(msgStr.c_str())) : false;
          }
      }
      msgStr = msg;
      size_t firstIndex = msgStr.find(",");
      if (firstIndex == std::string::npos) {
          LOGE("Set property fail for format wrong, msgStr = %s",  msgStr.c_str());
          return false;
      }
      size_t secondIndex = msgStr.find(",", firstIndex + 1);
      if (secondIndex == std::string::npos) {
          LOGE("Set property fail for format wrong, msgStr = %s",  msgStr.c_str());
          return false;
      }
      std::string name = msgStr.substr(firstIndex + 1, secondIndex - firstIndex - 1);
      std::string value = msgStr.substr(secondIndex + 1);
      int rs = property_set(const_cast<char *>(name.c_str()), const_cast<char *>(value.c_str()));
      LOGI("Set property name = %s, value = %s, rs = %d", name.c_str(), value.c_str(), rs);
      return rs == 0;
  }
  SendMessageToSocketClient(msg);
  return true;

}

void LogHidlService::SendMessageToSocketClient(const char* data) {
	LOGD("Send Messageto APK: %s, fd = %d.", data, clientConnect);

	char cmdAck[512];
	if (snprintf(cmdAck,sizeof(cmdAck)-1, "%s", data) < 0) {
        LOGE("sprintf error for %s", data);
    }
    
    int len = strlen(cmdAck);
    bool result = false;
    int retrycount = 5;

	const char *msg;
	msg = cmdAck;
	LOGD("Send Messageto APK cmdAck : %s", msg);
    pthread_mutex_lock(&mlock);
    do {
        if (clientConnect <= 0) {
            break;
        }
        int sendsize = write(clientConnect, msg, len);
        if (sendsize == len) {
            result = true;
            break;
        } else if (-1 == sendsize) {
            if (errno == EINTR) {
                continue;
            }
            LOGE("Failed to write socket, errno=%d", errno);
            result = false;
            break;
        }
        LOGE("incomplete write socket %d(%d:%d), and resend it.",
                clientConnect, sendsize, len);
        result = false;
    } while (retrycount-- > 0);
    pthread_mutex_unlock(&mlock);
    //SANITY TEST LOG LABEL. DONOT CHANGE
    LOGD("Send Message to client: [%s], result = %d", cmdAck, result);
}

}  // implementation
}  // namespace V1_0
}  // namespace log
}  // namespace hardware
}  // namespace mediatek
}  // namespace vendor

