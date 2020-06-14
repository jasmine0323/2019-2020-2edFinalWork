#include "widget.h"

#include <direct.h>
#include <windows.h>

#include <QBrush>
#include <QDebug>
#include <QDesktopServices>
#include <QFile>
#include <QFont>
#include <QMessageBox>
#include <QUrl>

#include "crawler.h"
#include "expr.h"
#include "rid.h"
#include "ui_widget.h"

Widget::Widget(QWidget *parent)
    : QWidget(parent),
      ui(new Ui::Widget),
      jieba(DICT_PATH, HMM_PATH, USER_DICT_PATH, IDF_PATH, STOP_WORD_PATH)
// jieba("dict/jieba.dict.utf8","dict/hmm_model.utf8","dict/user.dict.utf8","dict/idf.utf8","dict/stop_words.utf8")

//在构造类时对结巴进行初始化，提高效率
{
    ui->setupUi(this);

    ui->textBrowser->setAcceptRichText(true);
    ui->textBrowser->setOpenExternalLinks(true);

    ui->labelStatus->setText("主页");
    ui->progressBar->setMinimum(0);
    ui->progressBar->setMaximum(100);
    ui->progressBar->setValue(0);

    QFont font;
    font.setPointSize(12);
    font.setFamily(("Microsoft YaHei"));
    ui->textBrowser->setFont(font);

    //ui->textBrowser->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);    //设置垂直滚动条不可见

    connect(ui->lineEditTarget, SIGNAL(returnPressed()), ui->pushButtonSearch,
            SIGNAL(clicked()), Qt::UniqueConnection);

    /* 爬虫初始化 */
    initCrawler = false;

    /* 爬虫子线程 */
    crawlThread = new CrawlerThread;
    connect(this, SIGNAL(sendCrawlParameter(QString, int)), crawlThread,
            SLOT(slotCrawlParameter(QString, int)));
    connect(crawlThread, SIGNAL(crawlQuestion(QString, QString, int &)), this,
            SLOT(slotCrawlQuestion(QString, QString, int &)),
            Qt::BlockingQueuedConnection);
    connect(crawlThread, SIGNAL(crawlWarning(QString, QString)), this,
            SLOT(slotCrawlWarning(QString, QString)),
            Qt::BlockingQueuedConnection);
    connect(crawlThread, SIGNAL(crawlBrowserAppend(QString)), this,
            SLOT(slotCrawlBrowserAppend(QString)));
    connect(crawlThread, SIGNAL(crawlProgress(int)), this,
            SLOT(slotShowProgress(int)));
    connect(crawlThread, SIGNAL(crawlComplete(int)), this,
            SLOT(slotCrawlComplete(int)));

    /* 建立倒排索引子线程 */
    initRidThread = new InitRidThread;
    connect(this, SIGNAL(sendInitRidParameter(QString, int)), initRidThread,
            SLOT(slotInitRidParameter(QString, int)));
    connect(initRidThread, SIGNAL(showProgress(int)), this,
            SLOT(slotShowProgress(int)));
    connect(initRidThread, SIGNAL(initRidComplete(QString, int)), this,
            SLOT(slotInitRidComplete(QString, int)));
    // connect(initRidThread,SIGNAL(initRidComplete(string,int)),this,SLOT(slotInitRidComplete(string,int)));

    /* 初始化变量 */
    pageNumber = 0;
    working = false;
    //onLineinit();
    offLineinit();
}

Widget::~Widget() {
    crawlThread->exit();
    crawlThread->wait();
    delete crawlThread;
    crawlThread = nullptr;

    initRidThread->exit();
    initRidThread->wait();
    delete initRidThread;
    crawlThread = nullptr;

    RID_terminate();  //清空堆区内存
    delete ui;
}

void Widget::onLineinit(){
    if (working) {
        QMessageBox::warning(this, tr("请稍等"), tr("请等待当前进程结束"));
        return;
    }

    //爬取澎湃新闻的60个网页
    entry = "www.thepaper.cn";
    MAXFILECOUNT = 60;
    ui->textBrowser->clear();
    ui->labelStatus->setText("正在获取澎湃数据...");
    ui->progressBar->setMinimum(0);
    ui->progressBar->setMaximum(MAXFILECOUNT);
    ui->progressBar->setValue(0);
    pageNumber = 0;

    //初始化各种数据，只需初始化一次
    if (!initCrawler) {
        initCrawler = true;
        init_global_vars(MAXFILECOUNT);
    }

    //爬取数据
    working = true;
    //发送爬虫参数
    emit sendCrawlParameter(entry, MAXFILECOUNT);
    crawlThread->start();
}

void Widget::offLineinit(){
    if (working) {
        QMessageBox::warning(this, tr("请稍等"), tr("请等待当前进程结束"));
        return;
    }

    entry = "www.thepaper.cn";
    MAXFILECOUNT = 60;

    //只需初始化一次
    if (!initCrawler) {
        initCrawler = true;
        init_global_vars(MAXFILECOUNT);
    }
    pageNumber = init_website(entry.toStdString(), MAXFILECOUNT);
    init_matrix(pageNumber);
    pagerank(pageNumber);
    this->ui->labelStatus->setText("载入链接完成，共载入60个链接");
    init_rid();
}

void Widget::on_pushButtonSearch_clicked() {
    if (working) {
        QMessageBox::warning(this, tr("请稍等"), tr("请等待当前进程结束"));
        return;
    }

    QString targetSentence = ui->lineEditTarget->text().trimmed();
    if(targetSentence.isEmpty()){
        QMessageBox::warning(this, tr("错误"), tr("要搜索的结果为空"));
        return;
    }

    ui->textBrowser->clear();
    ui->labelStatus->setText("正在搜索...");

    /* 对目标进行分词 */
    vector<string> targetWords;
    string sentence = targetSentence.toStdString();
    jieba.CutForSearch(sentence, targetWords, true);

    /*for(int i=0;i<szTargetWords;i++)
        qDebug() << QString::fromStdString(targetWords[i]);*/

    /* 解析并转换成后缀,如果是布尔检索表达式则返回true,否则返回false */
    bool type = analysis_expr(targetSentence.toStdString());
    qDebug() << "type=" << type;

    if (type) {
        /* 布尔检索 */
        for (int i = 0; i < nr_post_token; i++) {
            /* 跳过操作符 */
            if (post_order_token[i].type != OP) {
                qDebug() << QString::fromStdString(post_order_token[i].str)
                         << endl;
                continue;
            }

            /* 对post_order_token[i]分词 */
            vector<string> tempWords;
            jieba.CutForSearch(post_order_token[i].str, tempWords, true);

            /* 将vector类型转为动态数组供倒排索引使用，并去掉空格 */
            int szTempWords = tempWords.size();
            int szRequest = 0;
            string *request = new string[szTempWords];
            for (int i = 0; i < szTempWords; i++)
                if (tempWords[i] != " ") {
                    request[szRequest] = tempWords[i];
                    szRequest++;
                }

            /* 对分词结果搜索，交集保存在post_order_token[i].arr，交集的大小保存在post_order_token[i].size
             */
            int *intersection;  //交集
            int szIntersection = 0;
            int *symDifference;  //对称差
            int szSymdifference = 0;
            RI_itsc_symdif(request, szRequest, intersection, szIntersection,
                           symDifference, szSymdifference);
            post_order_token[i].size = szIntersection;
            post_order_token[i].arr = intersection;

            for (int j = 0; j < szIntersection; j++)
                cout << post_order_token[i].arr[j] << " ";
            qDebug() << QString::fromStdString(post_order_token[i].str) << endl;
        }
        int szResultPages;  //最终结果数组的大小
        int *resultPages;   //最终的结果数组
        szResultPages = calc_post_order_expr(resultPages);
        for (int j = 0; j < szResultPages; j++) cout << resultPages[j] << " ";
        cout << endl;

        /* 显示搜索结果 */
        if (szResultPages <= 0) {
            ui->progressBar->setRange(0, 100);
            ui->progressBar->setValue(100);
            QMessageBox::information(this, tr("无结果"),
                                     tr("搜索完毕，未查到相关结果"));
        } else {
            ui->progressBar->setRange(0, szResultPages);
            ui->progressBar->setValue(0);
            show_results(resultPages, szResultPages, targetWords);
        }
    } else {
        /* 普通检索 */

        /* 将vector类型转为动态数组供倒排索引使用 */
        int szTargetWords = targetWords.size();
        int szRequest = 0;
        string *request = new string[szTargetWords];
        for (int i = 0; i < szTargetWords; i++)
            if (targetWords[i] != " ") {
                request[szRequest] = targetWords[i];
                szRequest++;
            }

        int *intersection;  //交集
        int szIntersection = 0;
        int *symDifference;  //对称差
        int szSymdifference = 0;
        int szUnion =
            RI_itsc_symdif(request, szRequest, intersection, szIntersection,
                           symDifference, szSymdifference);
        ui->labelStatus->setText("倒排索引检索完成");
        /*QMessageBox::information(this,tr("倒排索引"),
                                 tr("倒排索引检索完成，交集%1个，对称差%2个，并集%3个\n").arg(szIntersection).arg(szSymdifference).arg(szUnion));*/

        /* 显示搜索结果 */
        if (szUnion <= 0) {
            ui->progressBar->setRange(0, 100);
            ui->progressBar->setValue(100);
            QMessageBox::information(this, tr("无结果"),
                                     tr("搜索完毕，未查到相关结果"));
        } else {
            ui->progressBar->setRange(0, szUnion);
            ui->progressBar->setValue(0);
            show_results(intersection, szIntersection, targetWords);
            show_results(symDifference, szSymdifference, targetWords);
        }
    }

    ui->textBrowser->moveCursor(QTextCursor::Start);
    ui->labelStatus->setText("搜索完成");
}
