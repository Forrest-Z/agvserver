﻿#include <QString>
#include "IOCPModel.h"
#include "util/global.h"


// 每一个处理器上产生多少个线程(为了最大限度的提升服务器性能，详见配套文档)
#define WORKER_THREADS_PER_PROCESSOR 2
// 同时投递的Accept请求的数量(这个要根据实际的情况灵活设置)
#define MAX_POST_ACCEPT              10
// 传递给Worker线程的退出信号
#define EXIT_CODE                    NULL

// 释放指针宏
#define RELEASE(x)                      {if(x != NULL ){delete x;x=NULL;}}

// 释放句柄宏
#define RELEASE_HANDLE(x)               {if(x != NULL && x!=INVALID_HANDLE_VALUE){ CloseHandle(x);x = NULL;}}

// 释放Socket宏
#define RELEASE_SOCKET(x)               {if(x !=INVALID_SOCKET) { closesocket(x);x=INVALID_SOCKET;}}


CIOCPModel::CIOCPModel(void) :
    m_nThreads(0),
    m_hShutdownEvent(NULL),
    m_hIOCompletionPort(NULL),
    m_phWorkerThreads(NULL),
    m_nPort(DEFAULT_PORT),
    m_lpfnAcceptEx(NULL),
    m_pListenContext(NULL),
    recv_call_back(NULL)
{
}


CIOCPModel::~CIOCPModel(void)
{
    // 确保资源彻底释放
    this->Stop();
}

///////////////////////////////////////////////////////////////////
// 工作者线程：  为IOCP请求服务的工作者线程
//         也就是每当完成端口上出现了完成数据包，就将之取出来进行处理的线程
///////////////////////////////////////////////////////////////////

DWORD WINAPI CIOCPModel::_WorkerThread(LPVOID lpParam)
{
    THREADPARAMS_WORKER* pParam = (THREADPARAMS_WORKER*)lpParam;
    CIOCPModel* pIOCPModel = (CIOCPModel*)pParam->pIOCPModel;
    int nThreadNo = (int)pParam->nThreadNo;

    g_log->log(AGV_LOG_LEVEL_INFO,QStringLiteral("工作者线程启动，ID: ")+QString("%1").arg(nThreadNo));

    OVERLAPPED           *pOverlapped = NULL;
    PER_SOCKET_CONTEXT   *pSocketContext = NULL;
    DWORD                dwBytesTransfered = 0;

    // 循环处理请求，知道接收到Shutdown信息为止
    while (WAIT_OBJECT_0 != WaitForSingleObject(pIOCPModel->m_hShutdownEvent, 0))
    {
        BOOL bReturn = GetQueuedCompletionStatus(
                    pIOCPModel->m_hIOCompletionPort,
                    &dwBytesTransfered,
                    (PULONG_PTR)&pSocketContext,
                    &pOverlapped,
                    INFINITE);

        // 如果收到的是退出标志，则直接退出
        if (EXIT_CODE == (DWORD)pSocketContext)
        {
            break;
        }

        // 判断是否出现了错误
        if (!bReturn)
        {
            DWORD dwErr = GetLastError();

            // 显示一下提示信息
            if (!pIOCPModel->HandleError(pSocketContext, dwErr))
            {
                break;
            }

            continue;
        }
        else
        {
            // 读取传入的参数
            PER_IO_CONTEXT* pIoContext = CONTAINING_RECORD(pOverlapped, PER_IO_CONTEXT, m_Overlapped);

            // 判断是否有客户端断开了
            if ((0 == dwBytesTransfered) && (RECV_POSTED == pIoContext->m_OpType || SEND_POSTED == pIoContext->m_OpType))
            {
                QString ss = QString(QStringLiteral("客户端 %1:%2 断开连接.")).arg(inet_ntoa(pSocketContext->m_ClientAddr.sin_addr)).arg(ntohs(pSocketContext->m_ClientAddr.sin_port));
                g_log->log(AGV_LOG_LEVEL_INFO,ss);
                // 释放掉对应的资源
                pIOCPModel->_RemoveContext(pSocketContext);

                continue;
            }
            else
            {
                switch (pIoContext->m_OpType)
                {
                // Accept
                case ACCEPT_POSTED:
                {

                    // 为了增加代码可读性，这里用专门的_DoAccept函数进行处理连入请求
                    pIOCPModel->_DoAccpet(pSocketContext, pIoContext);


                }
                    break;

                    // RECV
                case RECV_POSTED:
                {
                    // 为了增加代码可读性，这里用专门的_DoRecv函数进行处理接收请求
                    pIOCPModel->_DoRecv(pSocketContext, pIoContext);
                }
                    break;

                    // SEND
                    // 这里略过不写了，要不代码太多了，不容易理解，Send操作相对来讲简单一些
                case SEND_POSTED:
                {

                }
                    break;
                default:
                    // 不应该执行到这里
                    g_log->log(AGV_LOG_LEVEL_ERROR,QStringLiteral("_WorkThread中的 pIoContext->m_OpType 参数异常."));
                    break;
                } //switch
            }//if
        }//if

    }//while

    QString ss = QString(QStringLiteral("工作者线程 %1 号退出.")).arg(nThreadNo);

    g_log->log(AGV_LOG_LEVEL_INFO,ss);

    // 释放线程参数
    RELEASE(lpParam);

    return 0;
}

////////////////////////////////////////////////////////////////////
// 初始化WinSock 2.2
bool CIOCPModel::LoadSocketLib()
{
    WSADATA wsaData;
    int nResult;
    nResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    // 错误(一般都不可能出现)
    if (NO_ERROR != nResult)
    {
        g_log->log(AGV_LOG_LEVEL_FATAL,QStringLiteral("初始化WinSock 2.2失败 "));
        return false;
    }

    return true;
}

//////////////////////////////////////////////////////////////////
//	启动服务器
bool CIOCPModel::Start(IOCP_RECV_CALL_BACK _recv_call_back, IOCP_DISCONNECT_CALL_BACK _disconnect_call_back)
{
    recv_call_back = _recv_call_back;
    disconnect_call_back = _disconnect_call_back;
    // 初始化线程互斥量
    InitializeCriticalSection(&m_csContextList);

    // 建立系统退出的事件通知
    m_hShutdownEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    // 初始化IOCP
    if (false == _InitializeIOCP())
    {
        g_log->log(AGV_LOG_LEVEL_FATAL,QStringLiteral("初始化IOCP失败 "));
        return false;
    }
    else
    {
        g_log->log(AGV_LOG_LEVEL_INFO,QStringLiteral("IOCP初始化完毕"));
    }

    // 初始化Socket
    if (false == _InitializeListenSocket())
    {
        g_log->log(AGV_LOG_LEVEL_FATAL,QStringLiteral("Listen Socket初始化失败"));
        this->_DeInitialize();
        return false;
    }
    else
    {
        g_log->log(AGV_LOG_LEVEL_INFO,QStringLiteral("Listen Socket初始化完毕"));
    }
    g_log->log(AGV_LOG_LEVEL_INFO,QStringLiteral("等候连接...."));

    return true;
}


////////////////////////////////////////////////////////////////////
//	开始发送系统退出消息，退出完成端口和线程资源
void CIOCPModel::Stop()
{
    if (m_pListenContext != NULL && m_pListenContext->m_Socket != INVALID_SOCKET)
    {
        // 激活关闭消息通知
        SetEvent(m_hShutdownEvent);

        for (int i = 0; i < m_nThreads; i++)
        {
            // 通知所有的完成端口操作退出
            PostQueuedCompletionStatus(m_hIOCompletionPort, 0, (DWORD)EXIT_CODE, NULL);
        }

        // 等待所有的客户端资源退出
        WaitForMultipleObjects(m_nThreads, m_phWorkerThreads, TRUE, INFINITE);

        // 清除客户端列表信息
        this->_ClearContextList();

        // 释放其他资源
        this->_DeInitialize();
        g_log->log(AGV_LOG_LEVEL_INFO,QStringLiteral("停止监听........"));
    }
}


////////////////////////////////
// 初始化完成端口
bool CIOCPModel::_InitializeIOCP()
{
    // 建立第一个完成端口
    m_hIOCompletionPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);

    if (NULL == m_hIOCompletionPort)
    {
        QString ss = QString(QStringLiteral("建立完成端口失败！错误代码:%1")).arg(WSAGetLastError());
        g_log->log(AGV_LOG_LEVEL_FATAL,ss);
        return false;
    }

    // 根据本机中的处理器数量，建立对应的线程数
    m_nThreads = WORKER_THREADS_PER_PROCESSOR * _GetNoOfProcessors();

    // 为工作者线程初始化句柄
    m_phWorkerThreads = new HANDLE[m_nThreads];

    // 根据计算出来的数量建立工作者线程
    DWORD nThreadID;
    for (int i = 0; i < m_nThreads; i++)
    {
        THREADPARAMS_WORKER* pThreadParams = new THREADPARAMS_WORKER;
        pThreadParams->pIOCPModel = this;
        pThreadParams->nThreadNo = i + 1;
        m_phWorkerThreads[i] = ::CreateThread(0, 0, _WorkerThread, (void *)pThreadParams, 0, &nThreadID);
    }

    QString ss = QString(QStringLiteral("建立 _WorkerThread %1 个")).arg(m_nThreads);
    g_log->log(AGV_LOG_LEVEL_FATAL,ss);

    return true;
}
// 获得本机的IP地址
QString CIOCPModel::GetLocalIP()
{
    QString strIp;
    // 获得本机主机名
    char hostname[MAX_PATH] = { 0 };
    gethostname(hostname, MAX_PATH);
    struct hostent FAR* lpHostEnt = gethostbyname(hostname);
    if (lpHostEnt == NULL)
    {
        return QStringLiteral("127.0.0.1");
    }

    // 取得IP地址列表中的第一个为返回的IP(因为一台主机可能会绑定多个IP)
    LPSTR lpAddr = lpHostEnt->h_addr_list[0];

    // 将IP地址转化成字符串形式
    struct in_addr inAddr;
    memmove(&inAddr, lpAddr, 4);
    strIp = QString(inet_ntoa(inAddr));

    return strIp;
}

/////////////////////////////////////////////////////////////////
// 初始化Socket
bool CIOCPModel::_InitializeListenSocket()
{
    // AcceptEx 和 GetAcceptExSockaddrs 的GUID，用于导出函数指针
    GUID GuidAcceptEx = WSAID_ACCEPTEX;
    GUID GuidGetAcceptExSockAddrs = WSAID_GETACCEPTEXSOCKADDRS;

    // 服务器地址信息，用于绑定Socket
    struct sockaddr_in ServerAddress;

    // 生成用于监听的Socket的信息
    m_pListenContext = new PER_SOCKET_CONTEXT;

    // 需要使用重叠IO，必须得使用WSASocket来建立Socket，才可以支持重叠IO操作
    m_pListenContext->m_Socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (INVALID_SOCKET == m_pListenContext->m_Socket)
    {
        QString ss = QString(QStringLiteral("初始化Socket失败，错误代码:%1")).arg(WSAGetLastError());
        g_log->log(AGV_LOG_LEVEL_FATAL,ss);

        return false;
    }
    else
    {
        g_log->log(AGV_LOG_LEVEL_INFO,QStringLiteral("WSASocket() 完成."));
    }

    // 将Listen Socket绑定至完成端口中
    if (NULL == CreateIoCompletionPort((HANDLE)m_pListenContext->m_Socket, m_hIOCompletionPort, (DWORD)m_pListenContext, 0))
    {
        QString ss = QString(QStringLiteral("绑定 Listen Socket至完成端口失败！错误代码:%1")).arg(WSAGetLastError());
        g_log->log(AGV_LOG_LEVEL_FATAL,ss);

        RELEASE_SOCKET(m_pListenContext->m_Socket);
        return false;
    }
    else
    {
        g_log->log(AGV_LOG_LEVEL_INFO,QStringLiteral("Listen Socket绑定完成端口 完成."));
    }

    // 填充地址信息
    ZeroMemory((char *)&ServerAddress, sizeof(ServerAddress));
    ServerAddress.sin_family = AF_INET;
    // 这里可以绑定任何可用的IP地址，或者绑定一个指定的IP地址
    //ServerAddress.sin_addr.s_addr = htonl(INADDR_ANY);
    ServerAddress.sin_addr.s_addr = inet_addr(GetLocalIP().toLocal8Bit().data());
    ServerAddress.sin_port = htons(m_nPort);

    // 绑定地址和端口
    if (SOCKET_ERROR == ::bind(m_pListenContext->m_Socket, (struct sockaddr *) &ServerAddress, sizeof(ServerAddress)))
    {
        QString ss = QString(QStringLiteral("bind()函数执行错误.错误代码:%1")).arg(WSAGetLastError());
        g_log->log(AGV_LOG_LEVEL_FATAL,ss);

        return false;
    }
    else
    {
        g_log->log(AGV_LOG_LEVEL_INFO,QStringLiteral("bind() 完成."));
    }

    // 开始进行监听
    if (SOCKET_ERROR == listen(m_pListenContext->m_Socket, SOMAXCONN))
    {
        QString ss = QString(QStringLiteral("Listen()函数执行出现错误..错误代码:%1")).arg(WSAGetLastError());
        g_log->log(AGV_LOG_LEVEL_FATAL,ss);
        return false;
    }
    else
    {
        g_log->log(AGV_LOG_LEVEL_INFO,QStringLiteral("Listen()完成."));
    }

    // 使用AcceptEx函数，因为这个是属于WinSock2规范之外的微软另外提供的扩展函数
    // 所以需要额外获取一下函数的指针，
    // 获取AcceptEx函数指针
    DWORD dwBytes = 0;
    if (SOCKET_ERROR == WSAIoctl(
                m_pListenContext->m_Socket,
                SIO_GET_EXTENSION_FUNCTION_POINTER,
                &GuidAcceptEx,
                sizeof(GuidAcceptEx),
                &m_lpfnAcceptEx,
                sizeof(m_lpfnAcceptEx),
                &dwBytes,
                NULL,
                NULL))
    {
        QString ss = QString(QStringLiteral("WSAIoctl 未能获取AcceptEx函数指针。.错误代码:%1")).arg(WSAGetLastError());
        g_log->log(AGV_LOG_LEVEL_FATAL,ss);
        this->_DeInitialize();
        return false;
    }

    // 获取GetAcceptExSockAddrs函数指针，也是同理
    if (SOCKET_ERROR == WSAIoctl(
                m_pListenContext->m_Socket,
                SIO_GET_EXTENSION_FUNCTION_POINTER,
                &GuidGetAcceptExSockAddrs,
                sizeof(GuidGetAcceptExSockAddrs),
                &m_lpfnGetAcceptExSockAddrs,
                sizeof(m_lpfnGetAcceptExSockAddrs),
                &dwBytes,
                NULL,
                NULL))
    {
        QString ss = QString(QStringLiteral("WSAIoctl 未能获取GuidGetAcceptExSockAddrs函数指针。错误代码:%1")).arg(WSAGetLastError());
        g_log->log(AGV_LOG_LEVEL_FATAL,ss);
        this->_DeInitialize();
        return false;
    }


    // 为AcceptEx 准备参数，然后投递AcceptEx I/O请求
    for (int i = 0;i < MAX_POST_ACCEPT;i++)
    {
        // 新建一个IO_CONTEXT
        PER_IO_CONTEXT* pAcceptIoContext = m_pListenContext->GetNewIoContext();

        if (false == this->_PostAccept(pAcceptIoContext))
        {
            m_pListenContext->RemoveContext(pAcceptIoContext);
            return false;
        }
    }
    QString ss = QString(QStringLiteral("投递 %1  个AcceptEx请求完毕")).arg(MAX_POST_ACCEPT);
    g_log->log(AGV_LOG_LEVEL_INFO,ss);
    return true;
}

////////////////////////////////////////////////////////////
//	最后释放掉所有资源
void CIOCPModel::_DeInitialize()
{
    // 删除客户端列表的互斥量
    DeleteCriticalSection(&m_csContextList);

    // 关闭系统退出事件句柄
    RELEASE_HANDLE(m_hShutdownEvent);

    // 释放工作者线程句柄指针
    for (int i = 0;i < m_nThreads;i++)
    {
        RELEASE_HANDLE(m_phWorkerThreads[i]);
    }

    RELEASE(m_phWorkerThreads);

    // 关闭IOCP句柄
    RELEASE_HANDLE(m_hIOCompletionPort);

    // 关闭监听Socket
    RELEASE(m_pListenContext);

    g_log->log(AGV_LOG_LEVEL_FATAL,QStringLiteral("释放资源完毕."));
}

//////////////////////////////////////////////////////////////////
// 投递Accept请求
bool CIOCPModel::_PostAccept(PER_IO_CONTEXT* pAcceptIoContext)
{
    // 准备参数
    DWORD dwBytes = 0;
    pAcceptIoContext->m_OpType = ACCEPT_POSTED;
    WSABUF *p_wbuf = &pAcceptIoContext->m_wsaBuf;
    OVERLAPPED *p_ol = &pAcceptIoContext->m_Overlapped;

    // 为以后新连入的客户端先准备好Socket( 这个是与传统accept最大的区别 )
    pAcceptIoContext->m_sockAccept = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (INVALID_SOCKET == pAcceptIoContext->m_sockAccept)
    {
        QString ss = QString(QStringLiteral("创建用于Accept的Socket失败！错误代码: %1")).arg(WSAGetLastError());
        g_log->log(AGV_LOG_LEVEL_INFO,ss);
        return false;
    }

    // 投递AcceptEx
    if (FALSE == m_lpfnAcceptEx(m_pListenContext->m_Socket, pAcceptIoContext->m_sockAccept, p_wbuf->buf, p_wbuf->len - ((sizeof(SOCKADDR_IN) + 16) * 2),
                                sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16, &dwBytes, p_ol))
    {
        if (WSA_IO_PENDING != WSAGetLastError())
        {
            QString ss = QString(QStringLiteral("投递AcceptEx 请求失败，错误代码: %1")).arg(WSAGetLastError());
            g_log->log(AGV_LOG_LEVEL_ERROR,ss);

            return false;
        }
    }

    return true;
}

////////////////////////////////////////////////////////////
// 在有客户端连入的时候，进行处理
// 流程有点复杂，你要是看不懂的话，就看配套的文档吧....
// 如果能理解这里的话，完成端口的机制你就消化了一大半了

// 总之你要知道，传入的是ListenSocket的Context，我们需要复制一份出来给新连入的Socket用
// 原来的Context还是要在上面继续投递下一个Accept请求
//
bool CIOCPModel::_DoAccpet(PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext)
{
    SOCKADDR_IN* ClientAddr = NULL;
    SOCKADDR_IN* LocalAddr = NULL;
    int remoteLen = sizeof(SOCKADDR_IN), localLen = sizeof(SOCKADDR_IN);

    ///////////////////////////////////////////////////////////////////////////
    // 1. 首先取得连入客户端的地址信息
    // 这个 m_lpfnGetAcceptExSockAddrs 不得了啊~~~~~~
    // 不但可以取得客户端和本地端的地址信息，还能顺便取出客户端发来的第一组数据，老强大了...
    this->m_lpfnGetAcceptExSockAddrs(pIoContext->m_wsaBuf.buf, pIoContext->m_wsaBuf.len - ((sizeof(SOCKADDR_IN) + 16) * 2),
                                     sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16, (LPSOCKADDR*)&LocalAddr, &localLen, (LPSOCKADDR*)&ClientAddr, &remoteLen);
    QString ss = QString(QStringLiteral("客户端 %1 : %2 连入")).arg(inet_ntoa(ClientAddr->sin_addr)).arg( ntohs(ClientAddr->sin_port));
    g_log->log(AGV_LOG_LEVEL_INFO,ss);
    //////////////////////////////////////////////////////////////////////////////////////////////////////
    // 2. 这里需要注意，这里传入的这个是ListenSocket上的Context，这个Context我们还需要用于监听下一个连接
    // 所以我还得要将ListenSocket上的Context复制出来一份为新连入的Socket新建一个SocketContext

    PER_SOCKET_CONTEXT* pNewSocketContext = new PER_SOCKET_CONTEXT;
    pNewSocketContext->m_Socket = pIoContext->m_sockAccept;
    memcpy(&(pNewSocketContext->m_ClientAddr), ClientAddr, sizeof(SOCKADDR_IN));

    // 参数设置完毕，将这个Socket和完成端口绑定(这也是一个关键步骤)
    if (false == this->_AssociateWithIOCP(pNewSocketContext))
    {
        RELEASE(pNewSocketContext);
        return false;
    }


    ///////////////////////////////////////////////////////////////////////////////////////////////////
    // 3. 继续，建立其下的IoContext，用于在这个Socket上投递第一个Recv数据请求
    PER_IO_CONTEXT* pNewIoContext = pNewSocketContext->GetNewIoContext();
    pNewIoContext->m_OpType = RECV_POSTED;
    pNewIoContext->m_sockAccept = pNewSocketContext->m_Socket;
    // 如果Buffer需要保留，就自己拷贝一份出来
    //memcpy( pNewIoContext->m_szBuffer,pIoContext->m_szBuffer,MAX_BUFFER_LEN );

    // 绑定完毕之后，就可以开始在这个Socket上投递完成请求了
    if (false == this->_PostRecv(pNewIoContext))
    {
        pNewSocketContext->RemoveContext(pNewIoContext);
        return false;
    }

    /////////////////////////////////////////////////////////////////////////////////////////////////
    // 4. 如果投递成功，那么就把这个有效的客户端信息，加入到ContextList中去(需要统一管理，方便释放资源)
    this->_AddToContextList(pNewSocketContext);

    ////////////////////////////////////////////////////////////////////////////////////////////////
    // 5. 使用完毕之后，把Listen Socket的那个IoContext重置，然后准备投递新的AcceptEx
    pIoContext->ResetBuffer();
    return this->_PostAccept(pIoContext);
}

////////////////////////////////////////////////////////////////////
// 投递接收数据请求
bool CIOCPModel::_PostRecv(PER_IO_CONTEXT* pIoContext)
{
    // 初始化变量
    DWORD dwFlags = 0;
    DWORD dwBytes = 0;
    WSABUF *p_wbuf = &pIoContext->m_wsaBuf;
    OVERLAPPED *p_ol = &pIoContext->m_Overlapped;

    pIoContext->ResetBuffer();
    pIoContext->m_OpType = RECV_POSTED;

    // 初始化完成后，，投递WSARecv请求
    int nBytesRecv = WSARecv(pIoContext->m_sockAccept, p_wbuf, 1, &dwBytes, &dwFlags, p_ol, NULL);

    // 如果返回值错误，并且错误的代码并非是Pending的话，那就说明这个重叠请求失败了
    if ((SOCKET_ERROR == nBytesRecv) && (WSA_IO_PENDING != WSAGetLastError()))
    {
        g_log->log(AGV_LOG_LEVEL_ERROR,QStringLiteral("投递第一个WSARecv失败！"));
        return false;
    }
    return true;
}

/////////////////////////////////////////////////////////////////
// 在有接收的数据到达的时候，进行处理
bool CIOCPModel::_DoRecv(PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext)
{
    // 先把上一次的数据显示出现，然后就重置状态，发出下一个Recv请求
    SOCKADDR_IN* ClientAddr = &pSocketContext->m_ClientAddr;
    QString sIp(inet_ntoa(ClientAddr->sin_addr));
    int port = ntohs(ClientAddr->sin_port);


    QString ss = QString(QStringLiteral("收到 %1 : %2 信息：%3")).arg(inet_ntoa(ClientAddr->sin_addr)).arg( ntohs(ClientAddr->sin_port)).arg(pIoContext->m_wsaBuf.buf);

    g_log->log(AGV_LOG_LEVEL_INFO,ss);

    if (recv_call_back != NULL) {
        recv_call_back(p_owner, pIoContext->m_wsaBuf.buf, pIoContext->m_wsaBuf.len,pSocketContext->m_Socket,sIp,port);
    }
    // 然后开始投递下一个WSARecv请求
    return _PostRecv(pIoContext);
}



/////////////////////////////////////////////////////
// 将句柄(Socket)绑定到完成端口中
bool CIOCPModel::_AssociateWithIOCP(PER_SOCKET_CONTEXT *pContext)
{
    // 将用于和客户端通信的SOCKET绑定到完成端口中
    HANDLE hTemp = CreateIoCompletionPort((HANDLE)pContext->m_Socket, m_hIOCompletionPort, (DWORD)pContext, 0);

    if (NULL == hTemp)
    {
        g_log->log(AGV_LOG_LEVEL_INFO,QStringLiteral("执行CreateIoCompletionPort()出现错误."));
        return false;
    }

    return true;
}


//////////////////////////////////////////////////////////////
// 将客户端的相关信息存储到数组中
void CIOCPModel::_AddToContextList(PER_SOCKET_CONTEXT *pHandleData)
{
    EnterCriticalSection(&m_csContextList);

    m_arrayClientContext.push_back(pHandleData);

    LeaveCriticalSection(&m_csContextList);
}

void CIOCPModel::_DoSend(char *buf, int len)
{
    EnterCriticalSection(&m_csContextList);
    for (QList<PER_SOCKET_CONTEXT*>::iterator itr = m_arrayClientContext.begin();itr != m_arrayClientContext.end();++itr)
    {
        ::send((*itr)->m_Socket, buf, len, 0);
    }
    LeaveCriticalSection(&m_csContextList);
}

void CIOCPModel::doSend(int socket,const char *buf,int len)
{
    EnterCriticalSection(&m_csContextList);
    for (QList<PER_SOCKET_CONTEXT*>::iterator itr = m_arrayClientContext.begin();itr != m_arrayClientContext.end();++itr)
    {
        if((*itr)->m_Socket == socket){
            ::send((*itr)->m_Socket, buf, len, 0);
            break;
        }
    }
    LeaveCriticalSection(&m_csContextList);
}

////////////////////////////////////////////////////////////////
//	移除某个特定的Context
void CIOCPModel::_RemoveContext(PER_SOCKET_CONTEXT *pSocketContext)
{
    EnterCriticalSection(&m_csContextList);

    for (QList<PER_SOCKET_CONTEXT*>::iterator itr = m_arrayClientContext.begin();itr != m_arrayClientContext.end();++itr)
    {
        if (pSocketContext == *itr)
        {
            RELEASE(pSocketContext);
            m_arrayClientContext.erase(itr);
            break;
        }
    }

    LeaveCriticalSection(&m_csContextList);
}

////////////////////////////////////////////////////////////////
// 清空客户端信息
void CIOCPModel::_ClearContextList()
{
    EnterCriticalSection(&m_csContextList);
    for (QList<PER_SOCKET_CONTEXT*>::iterator itr = m_arrayClientContext.begin();itr != m_arrayClientContext.end();++itr)
    {
        delete *itr;
    }

    m_arrayClientContext.clear();

    LeaveCriticalSection(&m_csContextList);
}


///////////////////////////////////////////////////////////////////
// 获得本机中处理器的数量
int CIOCPModel::_GetNoOfProcessors()
{
    SYSTEM_INFO si;

    GetSystemInfo(&si);

    return si.dwNumberOfProcessors;
}

/////////////////////////////////////////////////////////////////////
// 判断客户端Socket是否已经断开，否则在一个无效的Socket上投递WSARecv操作会出现异常
// 使用的方法是尝试向这个socket发送数据，判断这个socket调用的返回值
// 因为如果客户端网络异常断开(例如客户端崩溃或者拔掉网线等)的时候，服务器端是无法收到客户端断开的通知的

bool CIOCPModel::_IsSocketAlive(SOCKET s)
{
    int nByteSent = ::send(s, "", 0, 0);
    if (-1 == nByteSent) return false;
    return true;
}

///////////////////////////////////////////////////////////////////
// 显示并处理完成端口上的错误
bool CIOCPModel::HandleError(PER_SOCKET_CONTEXT *pContext, const DWORD& dwErr)
{
    // 如果是超时了，就再继续等吧
    if (WAIT_TIMEOUT == dwErr)
    {
        // 确认客户端是否还活着...
        if (!_IsSocketAlive(pContext->m_Socket))
        {
            g_log->log(AGV_LOG_LEVEL_TRACE,QStringLiteral("检测到客户端异常退出！"));
            if (disconnect_call_back != NULL) {
                SOCKADDR_IN* ClientAddr = &pContext->m_ClientAddr;
                QString sIp(inet_ntoa(ClientAddr->sin_addr));
                int port = ntohs(ClientAddr->sin_port);
                disconnect_call_back(p_owner,pContext->m_Socket,sIp,port);
            }

            this->_RemoveContext(pContext);
            return true;
        }
        else
        {
            g_log->log(AGV_LOG_LEVEL_TRACE,QStringLiteral("网络操作超时！重试中..."));
            return true;
        }
    }

    // 可能是客户端异常退出了
    else if (ERROR_NETNAME_DELETED == dwErr)
    {
        g_log->log(AGV_LOG_LEVEL_TRACE,QStringLiteral("检测到客户端异常退出！"));
        this->_RemoveContext(pContext);
        return true;
    }

    else
    {
        QString ss = QString(QStringLiteral("完成端口操作出现错误，线程退出。错误代码：%1")).arg((int)dwErr);
        g_log->log(AGV_LOG_LEVEL_ERROR,ss);
        return false;
    }
}




