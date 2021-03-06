#include "imap_mailbox.h"
#include "imap_parsers.h"
#include <QtAlgorithms>
#include <chrono>
imap::MailBox::MailBox(QObject *parent) : QObject(parent)
{
    conn = new Connection;
    conn->moveToThread(&netThread);
    connect(conn,&Connection::log,[&](QString s){
        emit log(s);
    });
    connect(conn,&Connection::error,[&](QString s){
        emit error(s);
    });
    connect(conn,&Connection::responseReady,this,&MailBox::getResponse);
    connect(this,&MailBox::sendRequest,conn,&Connection::send);
    connect(this,&MailBox::openConn,conn,&Connection::open);
    connect(&netThread,&QThread::finished,conn,&QObject::deleteLater);
    netThread.start();
}

void imap::MailBox::open(const QString& hostname)
{
    emit log("[[Mailbox]]:open request");
    emit openConn(hostname);
    logged_in.server=hostname;
}

imap::ResponseHandle imap::MailBox::login(QString username, QString password)
{
    auto req = newRequest(Command::login);
    req.data = makeReqStr(Command::login,username,password);
    emit log(QString{"[[Mailbox]]:sending LOGIN request"});
    emit sendRequest(req);
    return ResponseHandle{req.futureIndex,this};
}

imap::ResponseHandle imap::MailBox::send(imap::Command arglessCmd)
{
    auto req = newRequest(arglessCmd);
    req.data = makeReqStr(arglessCmd);
    emit log(QString{"[[Mailbox]]:sending %1 request"}.arg(imapCmds[arglessCmd].data()));
    emit sendRequest(req);
    return ResponseHandle{req.futureIndex,this};
}

imap::ResponseHandle imap::MailBox::select(QString folderName)
{
    auto req = newRequest(Command::select);
    putSafeRequest([req,folderName,this]()mutable
    {
        req.data=makeReqStr(Command::select,folderName);
        this->emit log(QString{"[[Mailbox]]:sending SELECT request"});
        this->emit sendRequest(req);
        safeState = false;
    });
    return ResponseHandle{req.futureIndex,this};
}

imap::ResponseHandle imap::MailBox::fetchInfo(int num, int skip)
{
    auto req = newRequest(Command::fetch,Context::fetch_envelope);
    putSafeRequest([num,skip,req,this]()mutable{

        auto start = QString::number(mailNum - skip);
        auto end = QString::number(mailNum - skip - num);
        auto range = start+':'+end;
        req.data=makeReqStr(Command::fetch,range,"(ENVELOPE","UID)");
        emit log("[[Mailbox]]:sending FETCH request");
        this->emit sendRequest(req);
    });
    return ResponseHandle{req.futureIndex,this};

}

imap::ResponseHandle imap::MailBox::fetchBody(QString uid)
{

    auto req = newRequest(Command::uid_fetch);
    req.data = makeReqStr(Command::uid_fetch,uid,"BODY[1]");
    emit log(QString{"[[Mailbox]]:sending UID FETCH request"});
    emit sendRequest(req);
    return ResponseHandle{req.futureIndex,this};
}

QVector<imap::MailEntry> imap::MailBox::getLatest(int num,int skip)
{
    emit log(QString{"[[MailBox]]: Trying to get %1 mails starting from %2"}.arg(num).arg(skip));
    std::partial_sort(mails.begin(),mails.begin()+num+skip,mails.end(),[](auto a,auto b){
        return a.details.date() > b.details.date();
    });
    QVector<MailEntry> ret;
    std::copy_n(mails.begin()+skip,num,std::back_inserter(ret));
    return ret;
}

QString imap::MailBox::getBody(QString uid)
{
    return mailBodies.take(uid.toInt());
}

imap::Message imap::MailBox::get(int index)
{
    if(responses.contains(index))
    {
        return responses.value(index);
    }
    else
    {
        getResponse(index);
        return responses.value(index);
    }
}

void imap::MailBox::putCallback(int index, const std::function<void (Message)>& func)
{
    callbacks.insert(index,func); //zastępujemy jeśli już było
}

imap::MailBox::~MailBox()
{
    netThread.exit();
    netThread.wait();
}
void imap::MailBox::getResponse(int index)
{

    auto future_iter = futures.find(index);
    const auto* future_entry = &future_iter->second;
    if(future_iter == futures.end() || !future_entry->future.valid())
    {
        //to oznacza ze już zajelisme sie tym przypadkiem
        //wczesniej, z funkcji get
        return;
    }
    auto val = future_iter->second.future.get(); //to wywołanie blokuje oczekując na odpowiedz
    responses.insert(index,val);
    auto responseBatch = val.Lines();

    emit log(QString("[[MailBox]]:Accepted response to ")+imapCmds[future_entry->cmd].data());

    auto bad = responseBatch.filter("BAD",Qt::CaseInsensitive);
    if(!bad.empty())
    {
        emit syntaxError();
    }

    //TODO jakoś to ulepszyć
    //obsługa różnych odpowiedzi na polecenia
    if(future_entry->cmd == Command::login)
    {
        //sprawdamy czy logowanie sie powiodło i potwierdzamy zalogowanie
        auto ok = responseBatch.filter("OK");
        if(!ok.empty())
        {
            emit loggedIn(logged_in);
        }
    }
    else if(future_entry->cmd == Command::select)
    {
        //wyciągamy informacje o folderze, updateujemy stan wewnętrzny
        //a potem przywracamy stan bezpieczny
        auto exists = responseBatch.filter("EXISTS");
        mailNum = parseLine(exists[0])[1].toInt();

        safeState = true;
    }
    else if(future_entry->cmd == Command::fetch)
    {
        if(future_entry->ctx == Context::fetch_envelope)
        {
            //parsujemy strukture "envelope"
            auto envelopes = responseBatch.filter("ENVELOPE");
            for(const auto& line : envelopes)
            {
                //taki ciąg zmiennych dla ułatwienia debugowania
                //bo to jedna z najbardziej bugogennych sekcji
                auto line_parts         = parseLine(line);
                auto line_response_data = line_parts.filter("ENVELOPE").first();
                auto response_list      = parseList(line_response_data);
                auto env_struct         = getFromList(response_list,"ENVELOPE");
                auto uid_val            = getFromList(response_list,"UID");
                MailEntry entry{uid_val,env_struct};

                emit log("Got mail: ("+entry.uid+") "+entry.details.from.mailAddress()+" - "+entry.details.subject);
                addMail(entry);
            }
            emit fetchReady();
        }
    }
    else if(future_entry->cmd == Command::uid_fetch)
    {
        auto list_str = responseBatch.filter("BODY")[0];
        auto list = parseLine(list_str);
        auto body_str = getFromList(list,"BODY[1]");
        auto uid = getFromList(list,"UID").toInt();

        mailBodies.insert(uid,body_str);

    }
    //wywołanie funkcji obsługującej
    if(auto call = callbacks.find(index); call != callbacks.end())
    {
        std::invoke(*call,val);
        callbacks.erase(call);
    }
    //kiedy stan jest bezpieczny a są jeszcze zakolejkowane wywołania
    //wtedy wywołaj wszystkie w kolejności
    while(!callQueue.empty() && safeState)
    {
        std::invoke(callQueue.takeFirst());
    }

}

void imap::MailBox::addMail(const imap::MailEntry& newEntry)
{
    auto uid = newEntry.uid.toInt();
    if(!uids.contains(uid))
    {
        uids.insert(uid);
        mails.push_back(newEntry);
    }

}

imap::Request imap::MailBox::newRequest(imap::Command cmd, Context ctx)
{
    Request req;
    req.promise = std::make_shared<std::promise<Message>>();
    futures.emplace(keycount,ResponseEntry{cmd,ctx,req.promise->get_future()});
    req.futureIndex = keycount;
    keycount++;
    return req;
}

imap::ResponseHandle &imap::ResponseHandle::onReady(std::function<void (Message)> func)
{
    myBox->putCallback(myIndex,std::move(func));
    return *this;
}

imap::ResponseHandle::ResponseHandle(int index, imap::MailBox *owner)
    :myIndex(index),myBox(owner){}
