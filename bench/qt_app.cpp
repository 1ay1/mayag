// bench/qt_app.cpp — Qt6 Widgets version of the comparison app.
//   qt_app [animate]
#include <QApplication>
#include <QLabel>
#include <QProgressBar>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QElapsedTimer>
#include <cmath>
#include <cstring>

int main(int argc, char** argv) {
    const bool animate = argc > 1 && std::strcmp(argv[1], "animate") == 0;
    int qargc = 1;
    QApplication app(qargc, argv);
    QWidget win;
    win.setWindowTitle("bench");
    win.resize(1000, 720);
    auto* box = new QVBoxLayout(&win);
    auto* title = new QLabel("Library");
    QFont f = title->font(); f.setPointSize(20); f.setBold(true); title->setFont(f);
    box->addWidget(title);
    auto* bar = new QProgressBar; bar->setRange(0, 1000);
    box->addWidget(bar);
    auto* inner = new QWidget;
    auto* list = new QVBoxLayout(inner);
    for (int i = 0; i < 200; ++i) {
        list->addWidget(new QLabel(QString("Track %1").arg(i)));
        list->addWidget(new QLabel("Artist name"));
    }
    auto* scroll = new QScrollArea; scroll->setWidget(inner); scroll->setWidgetResizable(true);
    box->addWidget(scroll);
    QElapsedTimer clock; clock.start();
    QTimer timer;
    if (animate) {
        QObject::connect(&timer, &QTimer::timeout, [&] {
            bar->setValue(static_cast<int>(500 + 500 * std::sin(clock.elapsed() / 1000.0 * 3.0)));
        });
        timer.setTimerType(Qt::PreciseTimer);
        timer.start(16);   // what QAbstractAnimation's driver uses
    }
    win.show();
    return app.exec();
}
