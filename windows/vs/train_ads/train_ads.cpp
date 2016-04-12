/*!
* Copyright (c) 2015 by Contributors
*/
#include <condition_variable>
#include <iostream>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <thread>
#include <numeric>
#include <chrono>
#include <windows.h>

#include "MxNetCpp.h"
#include "util.h"
#include "data.h"
#include "dmlc/io.h"
#include "io/filesys.h"

using namespace std;
using namespace mxnet::cpp;

class Mlp {
public:
    Mlp(bool _is_local_data)
        : ctx_cpu(Context(DeviceType::kCPU, 0)),
        ctx_dev(Context(DeviceType::kCPU, 0)), is_local(_is_local_data) {}
    size_t Run(KVStore *kv, std::unique_ptr<dmlc::SeekStream> stream, size_t streamSize) {

        /*define the symbolic net*/
        auto sym_x = Symbol::Variable("data");
        auto sym_label = Symbol::Variable("label");
        auto w1 = Symbol::Variable("w1");
        auto b1 = Symbol::Variable("b1");
        auto w2 = Symbol::Variable("w2");
        auto b2 = Symbol::Variable("b2");
        auto w3 = Symbol::Variable("w3");
        auto b3 = Symbol::Variable("b3");

        auto fc1 = FullyConnected("fc1", sym_x, w1, b1, 2048);
        auto act1 = Activation("act1", fc1, ActivationActType::relu);
        auto fc2 = FullyConnected("fc2", act1, w2, b2, 512);
        auto act2 = Activation("act2", fc2, ActivationActType::relu);
        auto fc3 = FullyConnected("fc3", act2, w3, b3, 1);
        auto mlp = LogisticRegressionOutput("softmax", fc3, sym_label);

        NDArray w1m(Shape(2048, 600), ctx_cpu),
            w2m(Shape(512, 2048), ctx_cpu),
            w3m(Shape(1, 512), ctx_cpu);
        NDArray::SampleGaussian(0, 1, &w1m);
        NDArray::SampleGaussian(0, 1, &w2m);
        NDArray::SampleGaussian(0, 1, &w3m);
        NDArray b1m(Shape(2048), ctx_cpu),
            b2m(Shape(512), ctx_cpu),
            b3m(Shape(1), ctx_cpu);
        NDArray::SampleGaussian(0, 1, &b1m);
        NDArray::SampleGaussian(0, 1, &b2m);
        NDArray::SampleGaussian(0, 1, &b3m);

        for (auto s : mlp.ListArguments()) {
            LG << s;
        }

        double samplesProcessed = 0;
        double sTime = get_time();

        /*setup basic configs*/
        std::unique_ptr<Optimizer> opt(new Optimizer("ccsgd", learning_rate, weight_decay));
        (*opt).SetParam("momentum", 0.9)
            .SetParam("rescale_grad", 1.0 / (kv->GetNumWorkers() * batchSize));
        //.SetParam("clip_gradient", 10);
        kv->SetOptimizer(std::move(opt));

        int rank = 0;
        int total_count = 1;
        if (is_local == false)
        {
            rank = kv->GetRank();
            total_count = kv->GetNumWorkers();
        }

        const int nMiniBatches = 1;
        bool init_kv = false;
        
        for (int ITER = 0; ITER < maxEpoch; ++ITER) {
            NDArray testData, testLabel;
            int mb = 0;
            size_t totalSamples = 0;
            DataReader dataReader(stream.get(), streamSize,
                sampleSize, rank, total_count, batchSize);            
            while (!dataReader.Eof()) {
                //if (mb++ >= nMiniBatches) break;
                // read data in
                auto r = dataReader.ReadBatch();
                size_t nSamples = r.size() / sampleSize;
                totalSamples += nSamples;
                vector<float> data_vec, label_vec;
                samplesProcessed += nSamples;
                CHECK(!r.empty());
                for (int i = 0; i < nSamples; i++) {
                    float * rp = r.data() + sampleSize * i;
                    label_vec.push_back(*rp);
                    data_vec.insert(data_vec.end(), rp + 1, rp + sampleSize);
                }
                r.clear();
                r.shrink_to_fit();

                const float *dptr = data_vec.data();
                const float *lptr = label_vec.data();
                NDArray dataArray = NDArray(Shape(nSamples, sampleSize - 1),
                    ctx_cpu, false);
                NDArray labelArray =
                    NDArray(Shape(nSamples), ctx_cpu, false);
                dataArray.SyncCopyFromCPU(dptr, nSamples * (sampleSize - 1));
                labelArray.SyncCopyFromCPU(lptr, nSamples);
                args_map["data"] = dataArray;
                args_map["label"] = labelArray;
                args_map["w1"] = w1m;
                args_map["b1"] = b1m;
                args_map["w2"] = w2m;
                args_map["b2"] = b2m;
                args_map["w3"] = w3m;
                args_map["b3"] = b3m;
                Executor *exe = mlp.SimpleBind(ctx_dev, args_map);
                std::vector<int> indices(exe->arg_arrays.size());
                std::iota(indices.begin(), indices.end(), 0);
                if (!init_kv) {
                    kv->Init(indices, exe->arg_arrays);
                    kv->Pull(indices, &exe->arg_arrays);
                    init_kv = true;
                }
                exe->Forward(true);
                NDArray::WaitAll();
                LG << "Iter " << ITER
                    << ", accuracy: " << Auc(exe->outputs[0], labelArray)
                    << "\t sample/s: " << samplesProcessed / (get_time() - sTime) 
                    << "\t Processing: [" << samplesProcessed * 100.0 / maxEpoch / dataReader.recordCount() << "%]";
                exe->Backward();
                kv->Push(indices, exe->grad_arrays);
                kv->Pull(indices, &exe->arg_arrays);
                //exe->UpdateAll(&opt, learning_rate);
                NDArray::WaitAll();
                delete exe;
            }
            LG << "Total samples: " << totalSamples;

            //LG << "Iter " << ITER
            //  << ", accuracy: " << ValAccuracy(mlp, testData, testLabel);
        }

        kv->Barrier();

        return samplesProcessed;
    }

private:
    Context ctx_cpu;
    Context ctx_dev;
    map<string, NDArray> args_map;
    const static int batchSize = 3072;
    const static int sampleSize = 601;
    const static int maxEpoch = 1;
    float learning_rate = 0.01;
    float weight_decay = 1e-5;
    bool is_local;

    float ValAccuracy(Symbol mlp,
        const NDArray& samples,
        const NDArray& labels) {
        size_t nSamples = samples.GetShape()[0];
        size_t nCorrect = 0;
        size_t startIndex = 0;
        args_map["data"] = samples;
        args_map["label"] = labels;

        Executor *exe = mlp.SimpleBind(ctx_dev, args_map);
        exe->Forward(false);
        const auto &out = exe->outputs;
        NDArray result = out[0].Copy(ctx_cpu);
        result.WaitToRead();
        const mx_float *pResult = result.GetData();
        const mx_float *pLabel = labels.GetData();
        for (int i = 0; i < nSamples; ++i) {
            float label = pLabel[i];
            int cat_num = result.GetShape()[1];
            float p_label = 0, max_p = pResult[i * cat_num];
            for (int j = 0; j < cat_num; ++j) {
                float p = pResult[i * cat_num + j];
                if (max_p < p) {
                    p_label = j;
                    max_p = p;
                }
            }
            if (label == p_label) nCorrect++;
        }
        delete exe;

        return nCorrect * 1.0 / nSamples;
    }

    float Auc(const NDArray& result, const NDArray& labels) {
        result.WaitToRead();
        const mx_float *pResult = result.GetData();
        const mx_float *pLabel = labels.GetData();
        int nSamples = labels.GetShape()[0];
        size_t nCorrect = 0;
        for (int i = 0; i < nSamples; ++i) {
            float label = pLabel[i];
            float p_label = pResult[i];
            if (label == (p_label >= 0.5)) nCorrect++;
        }
        return nCorrect * 1.0 / nSamples;
    }

};

/**
* Since embedded JVM doesn't support wildcard expansion in classpath,
* classpath needs to be expanded manually. The shell command to set
* expanded classpath may be too long for shell to handle. This function
* sets the long classpath in environmental variables.
*/
void init_env(bool use_hdfs) {
    std::string entry;
    char buf[129];
    if (use_hdfs) {
        entry = "CLASSPATH=";
        // Init classpath
        FILE* output = _popen("hadoop classpath --glob", "r");
        while (true)
        {
            size_t len = fread(buf, sizeof(char), sizeof(buf) - 1, output);
            if (len == 0)
                break;
            buf[len] = 0;
            entry += buf;
        }
        fclose(output);
        entry.pop_back(); // Remove line ending
        _putenv(entry.c_str());
    }

    // Init scheduler url
    if (GetEnvironmentVariableA("DMLC_PS_ROOT_URI", buf, sizeof(buf)) == 0) {
        std::ifstream in("scheduler_machine_list");
        std::string ip, port;
        in >> ip >> port;
        entry = "DMLC_PS_ROOT_URI=" + ip;
        _putenv(entry.c_str());
        entry = "DMLC_PS_ROOT_PORT=" + port;
        _putenv(entry.c_str());
    }
    LG << "Env inited";
}

int main(int argc, char const *argv[]) {
    using namespace dmlc::io;
    URI dataPath(argv[1]);
    init_env(dataPath.protocol == "hdfs://");

    KVStore *kv = new KVStore("dist_async");
    if (kv->GetRole() != "worker") {
        LG << "Running KVStore server";
        kv->RunServer();
        return 0;
    }    

    FileSystem* fs = FileSystem::GetInstance(dataPath);
    size_t size = fs->GetPathInfo(dataPath).size;
    std::unique_ptr<dmlc::SeekStream> stream(fs->OpenForRead(dataPath, false));

    Mlp mlp(dataPath.protocol.empty());
    auto start = std::chrono::steady_clock::now();
    auto sample_count = mlp.Run(kv, std::move(stream), size);
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>
        (std::chrono::steady_clock::now() - start);
    LG << "Training Duration = " << duration.count() / 1000.0 << "s\tlocal machine speed: [" << sample_count * 1000.0 / duration.count() << "/s]\ttotal speed: [" << sample_count * 1000.0 * kv->GetNumWorkers() / duration.count() << "/s]";
}