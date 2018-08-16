/*
 *    This software is in the public domain, furnished "as is", without technical
 *    support, and with no warranty, express or implied, as to its usefulness for
 *    any purpose.
 *
 */

#include <QtTest>
#include "syncenginetestutils.h"
#include <syncengine.h>

using namespace OCC;

SyncFileItemPtr findItem(const QSignalSpy &spy, const QString &path)
{
    for (const QList<QVariant> &args : spy) {
        auto item = args[0].value<SyncFileItemPtr>();
        if (item->destination() == path)
            return item;
    }
    return SyncFileItemPtr(new SyncFileItem);
}

bool itemInstruction(const QSignalSpy &spy, const QString &path, const csync_instructions_e instr)
{
    auto item = findItem(spy, path);
    return item->_instruction == instr;
}

SyncJournalFileRecord dbRecord(FakeFolder &folder, const QString &path)
{
    SyncJournalFileRecord record;
    folder.syncJournal().getFileRecord(path, &record);
    return record;
}

class TestSyncVirtualFiles : public QObject
{
    Q_OBJECT

private slots:
    void testVirtualFileLifecycle_data()
    {
        QTest::addColumn<bool>("doLocalDiscovery");

        QTest::newRow("full local discovery") << true;
        QTest::newRow("skip local discovery") << false;
    }

    void testVirtualFileLifecycle()
    {
        QFETCH(bool, doLocalDiscovery);

        FakeFolder fakeFolder{ FileInfo() };
        SyncOptions syncOptions;
        syncOptions._newFilesAreVirtual = true;
        fakeFolder.syncEngine().setSyncOptions(syncOptions);
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QSignalSpy completeSpy(&fakeFolder.syncEngine(), SIGNAL(itemCompleted(const SyncFileItemPtr &)));

        auto cleanup = [&]() {
            completeSpy.clear();
            if (!doLocalDiscovery)
                fakeFolder.syncEngine().setLocalDiscoveryOptions(LocalDiscoveryStyle::DatabaseAndFilesystem);
        };
        cleanup();

        // Create a virtual file for a new remote file
        fakeFolder.remoteModifier().mkdir("A");
        fakeFolder.remoteModifier().insert("A/a1", 64);
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(!fakeFolder.currentLocalState().find("A/a1"));
        QVERIFY(fakeFolder.currentLocalState().find("A/a1.owncloud"));
        QVERIFY(fakeFolder.currentRemoteState().find("A/a1"));
        QVERIFY(itemInstruction(completeSpy, "A/a1.owncloud", CSYNC_INSTRUCTION_NEW));
        QCOMPARE(dbRecord(fakeFolder, "A/a1.owncloud")._type, ItemTypeVirtualFile);
        cleanup();

        // Another sync doesn't actually lead to changes
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(!fakeFolder.currentLocalState().find("A/a1"));
        QVERIFY(fakeFolder.currentLocalState().find("A/a1.owncloud"));
        QVERIFY(fakeFolder.currentRemoteState().find("A/a1"));
        QCOMPARE(dbRecord(fakeFolder, "A/a1.owncloud")._type, ItemTypeVirtualFile);
        QVERIFY(completeSpy.isEmpty());
        cleanup();

        // Not even when the remote is rediscovered
        fakeFolder.syncJournal().forceRemoteDiscoveryNextSync();
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(!fakeFolder.currentLocalState().find("A/a1"));
        QVERIFY(fakeFolder.currentLocalState().find("A/a1.owncloud"));
        QVERIFY(fakeFolder.currentRemoteState().find("A/a1"));
        QCOMPARE(dbRecord(fakeFolder, "A/a1.owncloud")._type, ItemTypeVirtualFile);
        QVERIFY(completeSpy.isEmpty());
        cleanup();

        // Neither does a remote change
        fakeFolder.remoteModifier().appendByte("A/a1");
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(!fakeFolder.currentLocalState().find("A/a1"));
        QVERIFY(fakeFolder.currentLocalState().find("A/a1.owncloud"));
        QVERIFY(fakeFolder.currentRemoteState().find("A/a1"));
        QVERIFY(itemInstruction(completeSpy, "A/a1.owncloud", CSYNC_INSTRUCTION_UPDATE_METADATA));
        QCOMPARE(dbRecord(fakeFolder, "A/a1.owncloud")._type, ItemTypeVirtualFile);
        QCOMPARE(dbRecord(fakeFolder, "A/a1.owncloud")._fileSize, 65);
        cleanup();

        // If the local virtual file file is removed, it'll just be recreated
        if (!doLocalDiscovery)
            fakeFolder.syncEngine().setLocalDiscoveryOptions(LocalDiscoveryStyle::DatabaseAndFilesystem, { "A" });
        fakeFolder.localModifier().remove("A/a1.owncloud");
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(!fakeFolder.currentLocalState().find("A/a1"));
        QVERIFY(fakeFolder.currentLocalState().find("A/a1.owncloud"));
        QVERIFY(fakeFolder.currentRemoteState().find("A/a1"));
        QVERIFY(itemInstruction(completeSpy, "A/a1.owncloud", CSYNC_INSTRUCTION_NEW));
        QCOMPARE(dbRecord(fakeFolder, "A/a1.owncloud")._type, ItemTypeVirtualFile);
        QCOMPARE(dbRecord(fakeFolder, "A/a1.owncloud")._fileSize, 65);
        cleanup();

        // Remote rename is propagated
        fakeFolder.remoteModifier().rename("A/a1", "A/a1m");
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(!fakeFolder.currentLocalState().find("A/a1"));
        QVERIFY(!fakeFolder.currentLocalState().find("A/a1m"));
        QVERIFY(!fakeFolder.currentLocalState().find("A/a1.owncloud"));
        QVERIFY(fakeFolder.currentLocalState().find("A/a1m.owncloud"));
        QVERIFY(!fakeFolder.currentRemoteState().find("A/a1"));
        QVERIFY(fakeFolder.currentRemoteState().find("A/a1m"));
        QVERIFY(itemInstruction(completeSpy, "A/a1m.owncloud", CSYNC_INSTRUCTION_RENAME));
        QCOMPARE(dbRecord(fakeFolder, "A/a1m.owncloud")._type, ItemTypeVirtualFile);
        cleanup();

        // Remote remove is propagated
        fakeFolder.remoteModifier().remove("A/a1m");
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(!fakeFolder.currentLocalState().find("A/a1m.owncloud"));
        QVERIFY(!fakeFolder.currentRemoteState().find("A/a1m"));
        QVERIFY(itemInstruction(completeSpy, "A/a1m.owncloud", CSYNC_INSTRUCTION_REMOVE));
        QVERIFY(!dbRecord(fakeFolder, "A/a1.owncloud").isValid());
        QVERIFY(!dbRecord(fakeFolder, "A/a1m.owncloud").isValid());
        cleanup();

        // Edge case: Local virtual file but no db entry for some reason
        fakeFolder.remoteModifier().insert("A/a2", 64);
        fakeFolder.remoteModifier().insert("A/a3", 64);
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(fakeFolder.currentLocalState().find("A/a2.owncloud"));
        QVERIFY(fakeFolder.currentLocalState().find("A/a3.owncloud"));
        cleanup();

        fakeFolder.syncEngine().journal()->deleteFileRecord("A/a2.owncloud");
        fakeFolder.syncEngine().journal()->deleteFileRecord("A/a3.owncloud");
        fakeFolder.remoteModifier().remove("A/a3");
        fakeFolder.syncEngine().setLocalDiscoveryOptions(LocalDiscoveryStyle::FilesystemOnly);
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(fakeFolder.currentLocalState().find("A/a2.owncloud"));
        QVERIFY(itemInstruction(completeSpy, "A/a2.owncloud", CSYNC_INSTRUCTION_NEW));
        QVERIFY(dbRecord(fakeFolder, "A/a2.owncloud").isValid());
        QVERIFY(!fakeFolder.currentLocalState().find("A/a3.owncloud"));
        QVERIFY(!dbRecord(fakeFolder, "A/a3.owncloud").isValid());
        cleanup();
    }

    void testVirtualFileConflict()
    {
        FakeFolder fakeFolder{ FileInfo() };
        SyncOptions syncOptions;
        syncOptions._newFilesAreVirtual = true;
        fakeFolder.syncEngine().setSyncOptions(syncOptions);
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QSignalSpy completeSpy(&fakeFolder.syncEngine(), SIGNAL(itemCompleted(const SyncFileItemPtr &)));

        auto cleanup = [&]() {
            completeSpy.clear();
        };
        cleanup();

        // Create a virtual file for a new remote file
        fakeFolder.remoteModifier().mkdir("A");
        fakeFolder.remoteModifier().insert("A/a1", 64);
        fakeFolder.remoteModifier().insert("A/a2", 64);
        fakeFolder.remoteModifier().mkdir("B");
        fakeFolder.remoteModifier().insert("B/b1", 64);
        fakeFolder.remoteModifier().insert("B/b2", 64);
        fakeFolder.remoteModifier().mkdir("C");
        fakeFolder.remoteModifier().insert("C/c1", 64);
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(fakeFolder.currentLocalState().find("A/a1.owncloud"));
        QVERIFY(fakeFolder.currentLocalState().find("B/b2.owncloud"));
        cleanup();

        // A: the correct file and a conflicting file are added, virtual files stay
        // B: same setup, but the virtual files are deleted by the user
        // C: user adds a *directory* locally
        fakeFolder.localModifier().insert("A/a1", 64);
        fakeFolder.localModifier().insert("A/a2", 30);
        fakeFolder.localModifier().insert("B/b1", 64);
        fakeFolder.localModifier().insert("B/b2", 30);
        fakeFolder.localModifier().remove("B/b1.owncloud");
        fakeFolder.localModifier().remove("B/b2.owncloud");
        fakeFolder.localModifier().mkdir("C/c1");
        fakeFolder.localModifier().insert("C/c1/foo");
        QVERIFY(fakeFolder.syncOnce());

        // Everything is CONFLICT since mtimes are different even for a1/b1
        QVERIFY(itemInstruction(completeSpy, "A/a1", CSYNC_INSTRUCTION_CONFLICT));
        QVERIFY(itemInstruction(completeSpy, "A/a2", CSYNC_INSTRUCTION_CONFLICT));
        QVERIFY(itemInstruction(completeSpy, "B/b1", CSYNC_INSTRUCTION_CONFLICT));
        QVERIFY(itemInstruction(completeSpy, "B/b2", CSYNC_INSTRUCTION_CONFLICT));
        QVERIFY(itemInstruction(completeSpy, "C/c1", CSYNC_INSTRUCTION_CONFLICT));

        // no virtual file files should remain
        QVERIFY(!fakeFolder.currentLocalState().find("A/a1.owncloud"));
        QVERIFY(!fakeFolder.currentLocalState().find("A/a2.owncloud"));
        QVERIFY(!fakeFolder.currentLocalState().find("B/b1.owncloud"));
        QVERIFY(!fakeFolder.currentLocalState().find("B/b2.owncloud"));
        QVERIFY(!fakeFolder.currentLocalState().find("C/c1.owncloud"));

        // conflict files should exist
        QCOMPARE(fakeFolder.syncJournal().conflictRecordPaths().size(), 3);

        // nothing should have the virtual file tag
        QCOMPARE(dbRecord(fakeFolder, "A/a1")._type, ItemTypeFile);
        QCOMPARE(dbRecord(fakeFolder, "A/a2")._type, ItemTypeFile);
        QCOMPARE(dbRecord(fakeFolder, "B/b1")._type, ItemTypeFile);
        QCOMPARE(dbRecord(fakeFolder, "B/b2")._type, ItemTypeFile);
        QCOMPARE(dbRecord(fakeFolder, "C/c1")._type, ItemTypeFile);
        QVERIFY(!dbRecord(fakeFolder, "A/a1.owncloud").isValid());
        QVERIFY(!dbRecord(fakeFolder, "A/a2.owncloud").isValid());
        QVERIFY(!dbRecord(fakeFolder, "B/b1.owncloud").isValid());
        QVERIFY(!dbRecord(fakeFolder, "B/b2.owncloud").isValid());
        QVERIFY(!dbRecord(fakeFolder, "C/c1.owncloud").isValid());

        cleanup();
    }

    void testWithNormalSync()
    {
        FakeFolder fakeFolder{ FileInfo::A12_B12_C12_S12() };
        SyncOptions syncOptions;
        syncOptions._newFilesAreVirtual = true;
        fakeFolder.syncEngine().setSyncOptions(syncOptions);
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QSignalSpy completeSpy(&fakeFolder.syncEngine(), SIGNAL(itemCompleted(const SyncFileItemPtr &)));

        auto cleanup = [&]() {
            completeSpy.clear();
        };
        cleanup();

        // No effect sync
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        cleanup();

        // Existing files are propagated just fine in both directions
        fakeFolder.localModifier().appendByte("A/a1");
        fakeFolder.localModifier().insert("A/a3");
        fakeFolder.remoteModifier().appendByte("A/a2");
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        cleanup();

        // New files on the remote create virtual files
        fakeFolder.remoteModifier().insert("A/new");
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(!fakeFolder.currentLocalState().find("A/new"));
        QVERIFY(fakeFolder.currentLocalState().find("A/new.owncloud"));
        QVERIFY(fakeFolder.currentRemoteState().find("A/new"));
        QVERIFY(itemInstruction(completeSpy, "A/new.owncloud", CSYNC_INSTRUCTION_NEW));
        QCOMPARE(dbRecord(fakeFolder, "A/new.owncloud")._type, ItemTypeVirtualFile);
        cleanup();
    }

    void testVirtualFileDownload()
    {
        FakeFolder fakeFolder{ FileInfo() };
        SyncOptions syncOptions;
        syncOptions._newFilesAreVirtual = true;
        fakeFolder.syncEngine().setSyncOptions(syncOptions);
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QSignalSpy completeSpy(&fakeFolder.syncEngine(), SIGNAL(itemCompleted(const SyncFileItemPtr &)));

        auto cleanup = [&]() {
            completeSpy.clear();
        };
        cleanup();

        auto triggerDownload = [&](const QByteArray &path) {
            auto &journal = fakeFolder.syncJournal();
            SyncJournalFileRecord record;
            journal.getFileRecord(path + ".owncloud", &record);
            if (!record.isValid())
                return;
            record._type = ItemTypeVirtualFileDownload;
            journal.setFileRecord(record);
        };

        // Create a virtual file for remote files
        fakeFolder.remoteModifier().mkdir("A");
        fakeFolder.remoteModifier().insert("A/a1");
        fakeFolder.remoteModifier().insert("A/a2");
        fakeFolder.remoteModifier().insert("A/a3");
        fakeFolder.remoteModifier().insert("A/a4");
        fakeFolder.remoteModifier().insert("A/a5");
        fakeFolder.remoteModifier().insert("A/a6");
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(fakeFolder.currentLocalState().find("A/a1.owncloud"));
        QVERIFY(fakeFolder.currentLocalState().find("A/a2.owncloud"));
        QVERIFY(fakeFolder.currentLocalState().find("A/a3.owncloud"));
        QVERIFY(fakeFolder.currentLocalState().find("A/a4.owncloud"));
        QVERIFY(fakeFolder.currentLocalState().find("A/a5.owncloud"));
        QVERIFY(fakeFolder.currentLocalState().find("A/a6.owncloud"));
        cleanup();

        // Download by changing the db entry
        triggerDownload("A/a1");
        triggerDownload("A/a2");
        triggerDownload("A/a3");
        triggerDownload("A/a4");
        triggerDownload("A/a5");
        triggerDownload("A/a6");
        fakeFolder.remoteModifier().appendByte("A/a2");
        fakeFolder.remoteModifier().remove("A/a3");
        fakeFolder.remoteModifier().rename("A/a4", "A/a4m");
        fakeFolder.localModifier().insert("A/a5");
        fakeFolder.localModifier().insert("A/a6");
        fakeFolder.localModifier().remove("A/a6.owncloud");
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(itemInstruction(completeSpy, "A/a1", CSYNC_INSTRUCTION_NEW));
        QVERIFY(itemInstruction(completeSpy, "A/a1.owncloud", CSYNC_INSTRUCTION_NONE));
        QVERIFY(itemInstruction(completeSpy, "A/a2", CSYNC_INSTRUCTION_NEW));
        QVERIFY(itemInstruction(completeSpy, "A/a2.owncloud", CSYNC_INSTRUCTION_NONE));
        QVERIFY(itemInstruction(completeSpy, "A/a3.owncloud", CSYNC_INSTRUCTION_REMOVE));
        QVERIFY(itemInstruction(completeSpy, "A/a4m", CSYNC_INSTRUCTION_NEW));
        QVERIFY(itemInstruction(completeSpy, "A/a4.owncloud", CSYNC_INSTRUCTION_REMOVE));
        QVERIFY(itemInstruction(completeSpy, "A/a5", CSYNC_INSTRUCTION_CONFLICT));
        QVERIFY(itemInstruction(completeSpy, "A/a5.owncloud", CSYNC_INSTRUCTION_NONE));
        QVERIFY(itemInstruction(completeSpy, "A/a6", CSYNC_INSTRUCTION_CONFLICT));
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QCOMPARE(dbRecord(fakeFolder, "A/a1")._type, ItemTypeFile);
        QCOMPARE(dbRecord(fakeFolder, "A/a2")._type, ItemTypeFile);
        QVERIFY(!dbRecord(fakeFolder, "A/a3").isValid());
        QCOMPARE(dbRecord(fakeFolder, "A/a4m")._type, ItemTypeFile);
        QCOMPARE(dbRecord(fakeFolder, "A/a5")._type, ItemTypeFile);
        QCOMPARE(dbRecord(fakeFolder, "A/a6")._type, ItemTypeFile);
        QVERIFY(!dbRecord(fakeFolder, "A/a1.owncloud").isValid());
        QVERIFY(!dbRecord(fakeFolder, "A/a2.owncloud").isValid());
        QVERIFY(!dbRecord(fakeFolder, "A/a3.owncloud").isValid());
        QVERIFY(!dbRecord(fakeFolder, "A/a4.owncloud").isValid());
        QVERIFY(!dbRecord(fakeFolder, "A/a5.owncloud").isValid());
        QVERIFY(!dbRecord(fakeFolder, "A/a6.owncloud").isValid());
    }

    void testVirtualFileDownloadResume()
    {
        FakeFolder fakeFolder{ FileInfo() };
        SyncOptions syncOptions;
        syncOptions._newFilesAreVirtual = true;
        fakeFolder.syncEngine().setSyncOptions(syncOptions);
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QSignalSpy completeSpy(&fakeFolder.syncEngine(), SIGNAL(itemCompleted(const SyncFileItemPtr &)));

        auto cleanup = [&]() {
            completeSpy.clear();
            fakeFolder.syncJournal().wipeErrorBlacklist();
        };
        cleanup();

        auto triggerDownload = [&](const QByteArray &path) {
            auto &journal = fakeFolder.syncJournal();
            SyncJournalFileRecord record;
            journal.getFileRecord(path + ".owncloud", &record);
            if (!record.isValid())
                return;
            record._type = ItemTypeVirtualFileDownload;
            journal.setFileRecord(record);
            journal.avoidReadFromDbOnNextSync(record._path);
        };

        // Create a virtual file for remote files
        fakeFolder.remoteModifier().mkdir("A");
        fakeFolder.remoteModifier().insert("A/a1");
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(fakeFolder.currentLocalState().find("A/a1.owncloud"));
        cleanup();

        // Download by changing the db entry
        triggerDownload("A/a1");
        fakeFolder.serverErrorPaths().append("A/a1", 500);
        QVERIFY(!fakeFolder.syncOnce());
        QVERIFY(itemInstruction(completeSpy, "A/a1", CSYNC_INSTRUCTION_NEW));
        QVERIFY(itemInstruction(completeSpy, "A/a1.owncloud", CSYNC_INSTRUCTION_NONE));
        QVERIFY(fakeFolder.currentLocalState().find("A/a1.owncloud"));
        QVERIFY(!fakeFolder.currentLocalState().find("A/a1"));
        QCOMPARE(dbRecord(fakeFolder, "A/a1.owncloud")._type, ItemTypeVirtualFileDownload);
        QVERIFY(!dbRecord(fakeFolder, "A/a1").isValid());
        cleanup();

        fakeFolder.serverErrorPaths().clear();
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(itemInstruction(completeSpy, "A/a1", CSYNC_INSTRUCTION_NEW));
        QVERIFY(itemInstruction(completeSpy, "A/a1.owncloud", CSYNC_INSTRUCTION_NONE));
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QCOMPARE(dbRecord(fakeFolder, "A/a1")._type, ItemTypeFile);
        QVERIFY(!dbRecord(fakeFolder, "A/a1.owncloud").isValid());
    }

    // Check what might happen if an older sync client encounters virtual files
    void testOldVersion1()
    {
        FakeFolder fakeFolder{ FileInfo() };
        SyncOptions syncOptions;
        syncOptions._newFilesAreVirtual = true;
        fakeFolder.syncEngine().setSyncOptions(syncOptions);
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        // Create a virtual file
        fakeFolder.remoteModifier().mkdir("A");
        fakeFolder.remoteModifier().insert("A/a1");
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(fakeFolder.currentLocalState().find("A/a1.owncloud"));

        // Simulate an old client by switching the type of all ItemTypeVirtualFile
        // entries in the db to an invalid type.
        auto &db = fakeFolder.syncJournal();
        SyncJournalFileRecord rec;
        db.getFileRecord(QByteArray("A/a1.owncloud"), &rec);
        QVERIFY(rec.isValid());
        QCOMPARE(rec._type, ItemTypeVirtualFile);
        rec._type = static_cast<ItemType>(-1);
        db.setFileRecord(rec);

        // Also switch off new files becoming virtual files
        syncOptions._newFilesAreVirtual = false;
        fakeFolder.syncEngine().setSyncOptions(syncOptions);

        // A sync that doesn't do remote discovery has no effect
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(fakeFolder.currentLocalState().find("A/a1.owncloud"));
        QVERIFY(!fakeFolder.currentLocalState().find("A/a1"));
        QVERIFY(fakeFolder.currentRemoteState().find("A/a1"));
        QVERIFY(!fakeFolder.currentRemoteState().find("A/a1.owncloud"));

        // But with a remote discovery the virtual files will be removed and
        // the remote files will be downloaded.
        db.forceRemoteDiscoveryNextSync();
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(fakeFolder.currentLocalState().find("A/a1"));
        QVERIFY(!fakeFolder.currentLocalState().find("A/a1.owncloud"));
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
    }

    // Older versions may leave db entries for foo and foo.owncloud
    void testOldVersion2()
    {
        FakeFolder fakeFolder{ FileInfo() };

        // Sync a file
        fakeFolder.remoteModifier().mkdir("A");
        fakeFolder.remoteModifier().insert("A/a1");
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        // Create the virtual file too
        // In the wild, the new version would create the virtual file and the db entry
        // while the old version would download the plain file.
        fakeFolder.localModifier().insert("A/a1.owncloud");
        auto &db = fakeFolder.syncJournal();
        SyncJournalFileRecord rec;
        db.getFileRecord(QByteArray("A/a1"), &rec);
        rec._type = ItemTypeVirtualFile;
        rec._path = "A/a1.owncloud";
        db.setFileRecord(rec);

        SyncOptions syncOptions;
        syncOptions._newFilesAreVirtual = true;
        fakeFolder.syncEngine().setSyncOptions(syncOptions);

        // Check that a sync removes the virtual file and its db entry
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(!fakeFolder.currentLocalState().find("A/a1.owncloud"));
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QVERIFY(!dbRecord(fakeFolder, "A/a1.owncloud").isValid());
    }

    void testDownloadRecursive()
    {
        FakeFolder fakeFolder{ FileInfo() };
        SyncOptions syncOptions;
        syncOptions._newFilesAreVirtual = true;
        fakeFolder.syncEngine().setSyncOptions(syncOptions);
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        // Create a virtual file for remote files
        fakeFolder.remoteModifier().mkdir("A");
        fakeFolder.remoteModifier().mkdir("A/Sub");
        fakeFolder.remoteModifier().mkdir("A/Sub/SubSub");
        fakeFolder.remoteModifier().mkdir("A/Sub2");
        fakeFolder.remoteModifier().mkdir("B");
        fakeFolder.remoteModifier().mkdir("B/Sub");
        fakeFolder.remoteModifier().insert("A/a1");
        fakeFolder.remoteModifier().insert("A/a2");
        fakeFolder.remoteModifier().insert("A/Sub/a3");
        fakeFolder.remoteModifier().insert("A/Sub/a4");
        fakeFolder.remoteModifier().insert("A/Sub/SubSub/a5");
        fakeFolder.remoteModifier().insert("A/Sub2/a6");
        fakeFolder.remoteModifier().insert("B/b1");
        fakeFolder.remoteModifier().insert("B/Sub/b2");
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(fakeFolder.currentLocalState().find("A/a1.owncloud"));
        QVERIFY(fakeFolder.currentLocalState().find("A/a2.owncloud"));
        QVERIFY(fakeFolder.currentLocalState().find("A/Sub/a3.owncloud"));
        QVERIFY(fakeFolder.currentLocalState().find("A/Sub/a4.owncloud"));
        QVERIFY(fakeFolder.currentLocalState().find("A/Sub/SubSub/a5.owncloud"));
        QVERIFY(fakeFolder.currentLocalState().find("A/Sub2/a6.owncloud"));
        QVERIFY(fakeFolder.currentLocalState().find("B/b1.owncloud"));
        QVERIFY(fakeFolder.currentLocalState().find("B/Sub/b2.owncloud"));
        QVERIFY(!fakeFolder.currentLocalState().find("A/a1"));
        QVERIFY(!fakeFolder.currentLocalState().find("A/a2"));
        QVERIFY(!fakeFolder.currentLocalState().find("A/Sub/a3"));
        QVERIFY(!fakeFolder.currentLocalState().find("A/Sub/a4"));
        QVERIFY(!fakeFolder.currentLocalState().find("A/Sub/SubSub/a5"));
        QVERIFY(!fakeFolder.currentLocalState().find("A/Sub2/a6"));
        QVERIFY(!fakeFolder.currentLocalState().find("B/b1"));
        QVERIFY(!fakeFolder.currentLocalState().find("B/Sub/b2"));


        // Download All file in the directory A/Sub
        // (as in Folder::downloadVirtualFile)
        fakeFolder.syncJournal().markVirtualFileForDownloadRecursively("A/Sub");

        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(fakeFolder.currentLocalState().find("A/a1.owncloud"));
        QVERIFY(fakeFolder.currentLocalState().find("A/a2.owncloud"));
        QVERIFY(!fakeFolder.currentLocalState().find("A/Sub/a3.owncloud"));
        QVERIFY(!fakeFolder.currentLocalState().find("A/Sub/a4.owncloud"));
        QVERIFY(!fakeFolder.currentLocalState().find("A/Sub/SubSub/a5.owncloud"));
        QVERIFY(fakeFolder.currentLocalState().find("A/Sub2/a6.owncloud"));
        QVERIFY(fakeFolder.currentLocalState().find("B/b1.owncloud"));
        QVERIFY(fakeFolder.currentLocalState().find("B/Sub/b2.owncloud"));
        QVERIFY(!fakeFolder.currentLocalState().find("A/a1"));
        QVERIFY(!fakeFolder.currentLocalState().find("A/a2"));
        QVERIFY(fakeFolder.currentLocalState().find("A/Sub/a3"));
        QVERIFY(fakeFolder.currentLocalState().find("A/Sub/a4"));
        QVERIFY(fakeFolder.currentLocalState().find("A/Sub/SubSub/a5"));
        QVERIFY(!fakeFolder.currentLocalState().find("A/Sub2/a6"));
        QVERIFY(!fakeFolder.currentLocalState().find("B/b1"));
        QVERIFY(!fakeFolder.currentLocalState().find("B/Sub/b2"));

        // Add a file in a subfolder that was downloaded
        // Currently, this continue to add it as a virtual file.
        fakeFolder.remoteModifier().insert("A/Sub/SubSub/a7");
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(fakeFolder.currentLocalState().find("A/Sub/SubSub/a7.owncloud"));
        QVERIFY(!fakeFolder.currentLocalState().find("A/Sub/SubSub/a7"));

        // Now download all files in "A"
        fakeFolder.syncJournal().markVirtualFileForDownloadRecursively("A");
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(!fakeFolder.currentLocalState().find("A/a1.owncloud"));
        QVERIFY(!fakeFolder.currentLocalState().find("A/a2.owncloud"));
        QVERIFY(!fakeFolder.currentLocalState().find("A/Sub/a3.owncloud"));
        QVERIFY(!fakeFolder.currentLocalState().find("A/Sub/a4.owncloud"));
        QVERIFY(!fakeFolder.currentLocalState().find("A/Sub/SubSub/a5.owncloud"));
        QVERIFY(!fakeFolder.currentLocalState().find("A/Sub2/a6.owncloud"));
        QVERIFY(!fakeFolder.currentLocalState().find("A/Sub/SubSub/a7.owncloud"));
        QVERIFY(fakeFolder.currentLocalState().find("B/b1.owncloud"));
        QVERIFY(fakeFolder.currentLocalState().find("B/Sub/b2.owncloud"));
        QVERIFY(fakeFolder.currentLocalState().find("A/a1"));
        QVERIFY(fakeFolder.currentLocalState().find("A/a2"));
        QVERIFY(fakeFolder.currentLocalState().find("A/Sub/a3"));
        QVERIFY(fakeFolder.currentLocalState().find("A/Sub/a4"));
        QVERIFY(fakeFolder.currentLocalState().find("A/Sub/SubSub/a5"));
        QVERIFY(fakeFolder.currentLocalState().find("A/Sub2/a6"));
        QVERIFY(fakeFolder.currentLocalState().find("A/Sub/SubSub/a7"));
        QVERIFY(!fakeFolder.currentLocalState().find("B/b1"));
        QVERIFY(!fakeFolder.currentLocalState().find("B/Sub/b2"));

        // Now download remaining files in "B"
        fakeFolder.syncJournal().markVirtualFileForDownloadRecursively("B");
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
    }

    void testRenameToVirtual()
    {
        FakeFolder fakeFolder{ FileInfo::A12_B12_C12_S12() };
        SyncOptions syncOptions;
        syncOptions._newFilesAreVirtual = true;
        fakeFolder.syncEngine().setSyncOptions(syncOptions);
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QSignalSpy completeSpy(&fakeFolder.syncEngine(), SIGNAL(itemCompleted(const SyncFileItemPtr &)));

        auto cleanup = [&]() {
            completeSpy.clear();
        };
        cleanup();

        // If a file is renamed to <name>.owncloud, it becomes virtual
        fakeFolder.localModifier().rename("A/a1", "A/a1.owncloud");
        // If a file is renamed to <random>.owncloud, the file sticks around (to preserve user data)
        fakeFolder.localModifier().rename("A/a2", "A/rand.owncloud");
        QVERIFY(fakeFolder.syncOnce());

        QVERIFY(!fakeFolder.currentLocalState().find("A/a1"));
        QVERIFY(fakeFolder.currentLocalState().find("A/a1.owncloud"));
        QVERIFY(fakeFolder.currentRemoteState().find("A/a1"));
        QVERIFY(itemInstruction(completeSpy, "A/a1.owncloud", CSYNC_INSTRUCTION_NEW));
        QCOMPARE(dbRecord(fakeFolder, "A/a1.owncloud")._type, ItemTypeVirtualFile);

        QVERIFY(!fakeFolder.currentLocalState().find("A/a2"));
        QVERIFY(!fakeFolder.currentLocalState().find("A/a2.owncloud"));
        QVERIFY(fakeFolder.currentLocalState().find("A/rand.owncloud"));
        QVERIFY(!fakeFolder.currentRemoteState().find("A/a2"));
        QVERIFY(itemInstruction(completeSpy, "A/a2", CSYNC_INSTRUCTION_REMOVE));
        QVERIFY(!dbRecord(fakeFolder, "A/rand.owncloud").isValid());

        cleanup();
    }
};

QTEST_GUILESS_MAIN(TestSyncVirtualFiles)
#include "testsyncvirtualfiles.moc"
