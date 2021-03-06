#include "imap_connection.h"
#include <utility>
#include "imap_message.h"
#include "imap_parsers.h"
imap::Connection::Connection(QObject *parent) : QObject(parent),sock(this)
{
    connect(&sock,&QSslSocket::readyRead,this,&Connection::gotData);
    connect(&sock, QOverload<const QList<QSslError> &>::of(&QSslSocket::sslErrors),
        [=](const QList<QSslError> &errors){
        foreach(auto err,errors)
        {
          emit error(err.errorString());
        }
    });

}
imap::Connection::Connection(const QString& hostname, QObject *parent)
    :Connection(parent)
{
    open(hostname);
}

void imap::Connection::open(const QString& hostname)
{
    emit log("[[Connection]]:opening connection to"+hostname);
    sock.connectToHostEncrypted(hostname,SIMAPport);
    sock.waitForReadyRead(3000);
    if(sock.state() != QAbstractSocket::ConnectedState)
    {
        emit error("[[Connection]]: nie udalo sie polaczyc");
    }
}

void imap::Connection::close()
{
   sock.write(imap::makeReqStr(imap::Command::logout));
}

imap::Connection::~Connection()
{
    close();
}

void imap::Connection::gotData()
{
    auto data = sock.readAll();
    emit log("[[imap::Connection]]: Got:"+data);
    assembler.feed(data);

    if(assembler.isFinished())
    {
        emit log("[[imap::Connection]]: Accepted data");
        auto bytelist = assembler.outputLines;
        assembler.reset();
        //TODO sanitize/defer QString conversion
        QStringList strlist;
        strlist.reserve(bytelist.size());
        for(auto& ba : bytelist)
        {
            strlist.push_back(QString::fromUtf8(ba));
        }

        requestQueue.front().promise->set_value(strlist);
        emit responseReady(requestQueue.front().futureIndex);
        reqInProgress = false;
        requestQueue.pop_front();
        sendNext();
    }
    else
    {
        emit log("[[imap::Connection]]: Waiting for more data...");
    }
}

void imap::Connection::send(Request r)
{
    emit log("[[imap::Connection]]: Accepted send request ");
    requestQueue.push_back(std::move(r));
    if(!reqInProgress)sendNext();
}
void imap::Connection::sendNext()
{
    if(!requestQueue.empty() && !reqInProgress)
    {
        emit log("[[imap::Connection]]: Sending request");
        sock.write(requestQueue.front().data);
        reqInProgress=true;
    }
}
