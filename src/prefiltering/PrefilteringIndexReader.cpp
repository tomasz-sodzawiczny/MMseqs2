#include "PrefilteringIndexReader.h"
#include "DBWriter.h"
#include "Prefiltering.h"

const char*  PrefilteringIndexReader::CURRENT_VERSION="2.0.1";
unsigned int PrefilteringIndexReader::VERSION = 0;
unsigned int PrefilteringIndexReader::ENTRIES = 1;
unsigned int PrefilteringIndexReader::ENTRIESOFFSETS = 2;
unsigned int PrefilteringIndexReader::ENTRIESNUM = 3;
unsigned int PrefilteringIndexReader::SEQCOUNT = 4;
unsigned int PrefilteringIndexReader::META = 5;
unsigned int PrefilteringIndexReader::SEQINDEXDATA = 6;
unsigned int PrefilteringIndexReader::SEQINDEXDATASIZE = 7;
unsigned int PrefilteringIndexReader::SEQINDEXSEQOFFSET = 8;
bool PrefilteringIndexReader::checkIfIndexFile(DBReader<unsigned int>* reader) {
    char * version = reader->getDataByDBKey(VERSION);
    if(version == NULL){
        return false;
    }
    return (strncmp(version, CURRENT_VERSION, strlen(CURRENT_VERSION)) == 0 ) ? true : false;
}

void PrefilteringIndexReader::createIndexFile(std::string outDB, DBReader<unsigned int> *dbr,
                                              BaseMatrix * subMat, int maxSeqLen, bool hasSpacedKmer,
                                              bool compBiasCorrection,  int split, int alphabetSize, int kmerSize,
                                              bool diagonalScoring, int threads) {

    int splitCnt = split;
    if (splitCnt == 0){
        const size_t totalMemoryInByte =  Util::getTotalSystemMemory();
        std::pair<int, int> splitingSetting = Prefiltering::optimizeSplit(totalMemoryInByte, dbr, alphabetSize, kmerSize, threads);
        if(splitingSetting.second == -1){
            Debug(Debug::ERROR) << "Can not fit databased into " << totalMemoryInByte <<" byte. Please use a computer with more main memory.\n";
            EXIT(EXIT_FAILURE);
        }
        splitCnt = splitingSetting.second;
        if(kmerSize == 0){
            kmerSize = splitingSetting.first;
        }
    }
    
    if(kmerSize == 0){ // set k-mer based on aa size in database
        // if we have less than 10Mio * 335 amino acids use 6mers
        kmerSize = IndexTable::computeKmerSize(dbr->getAminoAcidDBSize());
    }
    Debug(Debug::INFO) << "Use kmer size " << kmerSize << " and split " << splitCnt << " using split mode\n";

    std::string outIndexName(outDB); // db.sk6
    std::string spaced = (hasSpacedKmer == true) ? "s" : "";
    outIndexName.append(".").append(spaced).append("k").append(SSTR(kmerSize));

    DBWriter writer(outIndexName.c_str(), std::string(outIndexName).append(".index").c_str(), DBWriter::BINARY_MODE);
    writer.open();

    Sequence seq(maxSeqLen, subMat->aa2int, subMat->int2aa, Sequence::AMINO_ACIDS, kmerSize, hasSpacedKmer, compBiasCorrection);

    for (int step = 0; step < splitCnt; step++) {
        size_t splitStart = 0;
        size_t splitSize  = 0;
        Util::decomposeDomainByAminoAcid(dbr->getAminoAcidDBSize(), dbr->getSeqLens(), dbr->getSize(),
                                         step, splitCnt, &splitStart, &splitSize);
        IndexTable *indexTable = new IndexTable(alphabetSize, kmerSize, false);
        Prefiltering::fillDatabase(dbr, &seq, indexTable, subMat, splitStart, splitStart + splitSize, diagonalScoring, threads);

        // save the entries
        std::string entries_key = SSTR(MathUtil::concatenate(ENTRIES, step));
        Debug(Debug::INFO) << "Write " << entries_key << "\n";
        char *entries = (char *) indexTable->getEntries();
        size_t entriesSize = indexTable->getTableEntriesNum() * indexTable->getSizeOfEntry();
        writer.writeData(entries, entriesSize, entries_key.c_str(), 0);

        // save the size
        std::string entriesoffsets_key = SSTR(MathUtil::concatenate(ENTRIESOFFSETS, step));
        Debug(Debug::INFO) << "Write " << entriesoffsets_key << "\n";

        char *offsets = (char*)indexTable->getOffsets();
        size_t offsetsSize = (indexTable->getTableSize() + 1) * sizeof(size_t);
        writer.writeData(offsets, offsetsSize, entriesoffsets_key.c_str(), 0);
        indexTable->deleteEntries();

        SequenceLookup *lookup = indexTable->getSequenceLookup();
        std::string seqindexdata_key = SSTR(MathUtil::concatenate(SEQINDEXDATA, step));
        Debug(Debug::INFO) << "Write " << seqindexdata_key << "\n";
        writer.writeData(lookup->getData(), lookup->getDataSize(), seqindexdata_key.c_str(), 0);

        std::string seqindex_datasize_key = SSTR(MathUtil::concatenate(SEQINDEXDATASIZE, step));
        Debug(Debug::INFO) << "Write " << seqindex_datasize_key << "\n";
        int64_t seqindexDataSize = lookup->getDataSize();
        char *seqindexDataSizePtr = (char *) &seqindexDataSize;
        writer.writeData(seqindexDataSizePtr, 1 * sizeof(int64_t), seqindex_datasize_key.c_str(), 0);

        size_t *sequenceOffsets = lookup->getOffsets();
        size_t sequenceCount = lookup->getSequenceCount();
        std::string seqindex_seqoffset = SSTR(MathUtil::concatenate(SEQINDEXSEQOFFSET, step));
        Debug(Debug::INFO) << "Write " << seqindex_seqoffset << "\n";
        writer.writeData((char *) sequenceOffsets, (sequenceCount + 1) * sizeof(size_t), seqindex_seqoffset.c_str(), 0);

        // meta data
        // ENTRIESNUM
        std::string entriesnum_key = SSTR(MathUtil::concatenate(ENTRIESNUM, step));
        Debug(Debug::INFO) << "Write " << entriesnum_key << "\n";
        uint64_t entriesNum = indexTable->getTableEntriesNum();
        char *entriesNumPtr = (char *) &entriesNum;
        writer.writeData(entriesNumPtr, 1 * sizeof(int64_t), entriesnum_key.c_str(), 0);
        // SEQCOUNT
        std::string tablesize_key = SSTR(MathUtil::concatenate(SEQCOUNT, step));
        Debug(Debug::INFO) << "Write " << tablesize_key << "\n";
        size_t tablesize = {indexTable->getSize()};
        char *tablesizePtr = (char *) &tablesize;
        writer.writeData(tablesizePtr, 1 * sizeof(size_t), tablesize_key.c_str(), 0);

        delete indexTable;
    }
    Debug(Debug::INFO) << "Write " << META << "\n";
    int local = 1;
    int spacedKmer = (hasSpacedKmer) ? 1 : 0;
    int metadata[] = {kmerSize, alphabetSize, 0, split, local, spacedKmer};
    char *metadataptr = (char *) &metadata;
    writer.writeData(metadataptr, 6 * sizeof(int), SSTR(META).c_str(), 0);

    Debug(Debug::INFO) << "Write " << VERSION << "\n";
    writer.writeData((char *) CURRENT_VERSION, strlen(CURRENT_VERSION) * sizeof(char), SSTR(VERSION).c_str(), 0);

    Debug(Debug::INFO) << "Write MMSEQSFFINDEX \n";
    std::ifstream src(dbr->getIndexFileName(), std::ios::binary);
    std::ofstream dst(std::string(outIndexName + ".mmseqsindex").c_str(), std::ios::binary);
    dst << src.rdbuf();
    src.close();
    dst.close();
    writer.close();
    Debug(Debug::INFO) << "Done. \n";
}

DBReader<unsigned int>*PrefilteringIndexReader::openNewReader(DBReader<unsigned int>*dbr) {
    std::string filePath(dbr->getDataFileName());
    std::string fullPath = filePath + std::string(".mmseqsindex");
    DBReader<unsigned int>*reader = new DBReader<unsigned int>("", fullPath.c_str(), DBReader<unsigned int>::USE_INDEX);
    reader->open(DBReader<unsigned int>::NOSORT);
    return reader;
}

IndexTable *PrefilteringIndexReader::generateIndexTable(DBReader<unsigned int>*dbr, int split, bool diagonalScoring) {
    int64_t   entriesNum    = *((int64_t *) dbr->getDataByDBKey(MathUtil::concatenate(ENTRIESNUM, split)));
    size_t sequenceCount    = *((size_t *)dbr->getDataByDBKey(MathUtil::concatenate(SEQCOUNT, split)));
    PrefilteringIndexData data = getMetadata(dbr);

    char * entriesData = dbr->getDataByDBKey(MathUtil::concatenate(ENTRIES, split));
    char * entriesOffsetsData = dbr->getDataByDBKey(MathUtil::concatenate(ENTRIESOFFSETS, split));

    char * seqData    = dbr->getDataByDBKey(MathUtil::concatenate(SEQINDEXDATA, split));

    size_t seqOffsetsId = dbr->getId(MathUtil::concatenate(SEQINDEXSEQOFFSET, split));
    char * seqOffsetsData     = dbr->getData(seqOffsetsId);
    size_t seqOffsetLength    = dbr->getSeqLens(seqOffsetsId);

    IndexTable *retTable;
    if (data.local) {
        retTable = new IndexTable(data.alphabetSize, data.kmerSize, true);
    }else {
        Debug(Debug::ERROR) << "Seach mode is not valid.\n";
        EXIT(EXIT_FAILURE);
    }

    SequenceLookup * sequenceLookup = NULL;
    if(diagonalScoring == true) {
        sequenceLookup = new SequenceLookup(sequenceCount);
        sequenceLookup->initLookupByExternalData(seqData, seqOffsetLength, (size_t *) seqOffsetsData);
    }
    retTable->initTableByExternalData(sequenceCount, entriesNum,
                                      (IndexEntryLocal*) entriesData, (size_t *)entriesOffsetsData, sequenceLookup);
    return retTable;
}


PrefilteringIndexData PrefilteringIndexReader::getMetadata(DBReader<unsigned int>*dbr) {
    PrefilteringIndexData prefData;
    int *version_tmp = (int *) dbr->getDataByDBKey(VERSION);
    Debug(Debug::INFO) << "Index version: " << version_tmp[0] << "\n";
    int *metadata_tmp = (int *) dbr->getDataByDBKey(META);

    Debug(Debug::INFO) << "KmerSize:     " << metadata_tmp[0] << "\n";
    Debug(Debug::INFO) << "AlphabetSize: " << metadata_tmp[1] << "\n";
    Debug(Debug::INFO) << "Skip:         " << metadata_tmp[2] << "\n";
    Debug(Debug::INFO) << "Split:        " << metadata_tmp[3] << "\n";
    Debug(Debug::INFO) << "Type:         " << metadata_tmp[4] << "\n";
    Debug(Debug::INFO) << "Spaced:       " << metadata_tmp[5] << "\n";

    prefData.kmerSize = metadata_tmp[0];
    prefData.alphabetSize = metadata_tmp[1];
    prefData.skip = metadata_tmp[2];
    prefData.split = metadata_tmp[3];
    prefData.local = metadata_tmp[4];
    prefData.spacedKmer = metadata_tmp[5];

    return prefData;
}
