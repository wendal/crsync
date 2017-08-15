TEMPLATE = app
CONFIG += console
CONFIG -= app_bundle
CONFIG -= qt

TARGET = crsync
DESTDIR = ../dst/

SOURCES += \
    digest.c \
    diff.c \
    patch.c \
    http.c \
    magnet.c \
    helper.c \
    util.c \
    log.c \
    crsync.c \
    crsync-console.c \
    ../extra/md5.c \
    ../extra/tpl.c \
    ../extra/dictionary.c \
    ../extra/iniparser.c

HEADERS += \
    global.h \
    digest.h \
    diff.h \
    patch.h \
    http.h \
    magnet.h \
    helper.h \
    util.h \
    log.h \
    crsync.h \
    crsyncver.h \
    ../extra/md5.h \
    ../extra/tpl.h \
    ../extra/uthash.h \
    ../extra/utstring.h \
    ../extra/utlist.h \
    ../extra/dictionary.h \
    ../extra/iniparser.h

DEFINES += HASH_BLOOM=21 CURL_STATICLIB
INCLUDEPATH += $${_PRO_FILE_PWD_}/../libcurl/include
LIBS += -L$${_PRO_FILE_PWD_}/../libcurl/lib/m64 -lcurl -lz -lssl -lcrypto -ldl


INCLUDEPATH += $${_PRO_FILE_PWD_}/../extra/

QMAKE_CFLAGS += -O3 -std=c99 -fopenmp
QMAKE_LFLAGS += -static -fopenmp

win32:RC_FILE = crsync.rc

include(deployment.pri)
qtcAddDeployment()
