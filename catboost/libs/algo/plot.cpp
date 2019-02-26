#include "plot.h"

#include "apply.h"

#include <catboost/libs/loggers/catboost_logger_helpers.h>
#include <catboost/libs/loggers/logger.h>
#include <catboost/libs/logging/logging.h>
#include <catboost/libs/options/json_helper.h>

#include <util/folder/path.h>
#include <util/generic/array_ref.h>
#include <util/generic/guid.h>
#include <util/generic/utility.h>
#include <util/generic/xrange.h>
#include <util/stream/fwd.h>
#include <util/string/builder.h>
#include <util/system/file.h>
#include <util/system/yassert.h>
#include <util/ysaveload.h>

#include <cmath>


using namespace NCB;


TMetricsPlotCalcer::TMetricsPlotCalcer(
    const TFullModel& model,
    const TVector<THolder<IMetric>>& metrics,
    NPar::TLocalExecutor& executor,
    const TString& tmpDir,
    ui32 first,
    ui32 last,
    ui32 step,
    ui32 processIterationStep)
    : Model(model)
    , Executor(executor)
    , First(first)
    , Last(last)
    , Step(step)
    , TmpDir(tmpDir)
    , ProcessedIterationsCount(0)
    , ProcessedIterationsStep(processIterationStep)
{
    EnsureCorrectParams();
    for (ui32 iteration = First; iteration < Last; iteration += Step) {
        Iterations.push_back(iteration);
    }
    if (Iterations.back() != Last - 1) {
        Iterations.push_back(Last - 1);
    }
    for (int metricIndex = 0; metricIndex < metrics.ysize(); ++metricIndex) {
        const auto& metric = metrics[metricIndex];
        if (metric->IsAdditiveMetric()) {
            AdditiveMetrics.push_back(metric.Get());
            AdditiveMetricsIndices.push_back(metricIndex);
        }
        else {
            NonAdditiveMetrics.push_back(metric.Get());
            NonAdditiveMetricsIndices.push_back(metricIndex);
            CB_ENSURE(metric->GetErrorType() == EErrorType::PerObjectError,
                "Error: we don't support non-additive querywise and pairwise metrics currenty");
        }
    }
    AdditiveMetricPlots.resize(AdditiveMetrics.ysize(), TVector<TMetricHolder>(Iterations.ysize()));
    NonAdditiveMetricPlots.resize(NonAdditiveMetrics.ysize(), TVector<TMetricHolder>(Iterations.ysize()));
}

void TMetricsPlotCalcer::ComputeAdditiveMetric(
    const TVector<TVector<double>>& approx,
    TConstArrayRef<float> target,
    TConstArrayRef<float> weights,
    TConstArrayRef<TQueryInfo> queriesInfo,
    ui32 plotLineIndex
) {
    for (ui32 metricId = 0; metricId < AdditiveMetrics.size(); ++metricId) {
        const auto& metric = *AdditiveMetrics[metricId];
        const auto docCount = static_cast<int>(target.size());
        const auto queryCount = static_cast<int>(queriesInfo.size());
        TMetricHolder metricResult;
        if (metric.GetErrorType() == EErrorType::PerObjectError) {
            metricResult = metric.Eval(approx, target, weights, queriesInfo, 0, docCount, Executor);
        } else {
            CB_ENSURE(metric.GetErrorType() == EErrorType::QuerywiseError || metric.GetErrorType() == EErrorType::PairwiseError);
            metricResult = metric.Eval(approx, target, weights, queriesInfo, 0, queryCount, Executor);
        }
        AdditiveMetricPlots[metricId][plotLineIndex].Add(metricResult);
    }
}

void TMetricsPlotCalcer::Append(const TVector<TVector<double>>& approx, TVector<TVector<double>>* dst, int dstStartDoc) {
    const ui32 docCount = approx[0].size();

    for (ui32 dim = 0; dim < approx.size(); ++dim) {
        NPar::ParallelFor(Executor, 0, docCount, [&](int i) {
            (*dst)[dim][dstStartDoc + i] += approx[dim][i];
        });
    };
}

TMetricsPlotCalcer& TMetricsPlotCalcer::ProceedDataSetForAdditiveMetrics(const TProcessedDataProvider& processedData) {
    ProceedDataSet(processedData, 0, Iterations.ysize(), /*isAdditiveMetrics=*/true);
    return *this;
}

TMetricsPlotCalcer& TMetricsPlotCalcer::ProceedDataSetForNonAdditiveMetrics(const TProcessedDataProvider& processedData) {
    if (ProcessedIterationsCount == 0) {
        const ui32 newPoolSize = NonAdditiveMetricsData.Target.size() + processedData.ObjectsData->GetObjectCount();
        NonAdditiveMetricsData.Target.reserve(newPoolSize);
        NonAdditiveMetricsData.Weights.reserve(newPoolSize);

        const auto target = GetTarget(processedData.TargetData);
        NonAdditiveMetricsData.Target.insert(NonAdditiveMetricsData.Target.end(), target.begin(), target.end());

        const auto weights = GetWeights(processedData.TargetData);
        NonAdditiveMetricsData.Weights.insert(NonAdditiveMetricsData.Weights.end(), weights.begin(), weights.end());
    }
    ui32 begin = ProcessedIterationsCount;
    ui32 end = Min<ui32>(ProcessedIterationsCount + ProcessedIterationsStep, Iterations.size());
    ProceedDataSet(processedData, begin, end, /*isAdditiveMetrics=*/false);
    return *this;
}

TMetricsPlotCalcer& TMetricsPlotCalcer::FinishProceedDataSetForNonAdditiveMetrics() {
    ui32 begin = ProcessedIterationsCount;
    ui32 end = Min<ui32>(ProcessedIterationsCount + ProcessedIterationsStep, Iterations.size());
    ComputeNonAdditiveMetrics(begin, end);
    ProcessedIterationsCount = end;
    if (AreAllIterationsProcessed()) {
        DeleteApprox(end - 1);
    } else {
        LastApproxes = MakeHolder<TIFStream>(GetApproxFileName(end - 1));
    }
    return *this;
}

static void Load(ui32 docCount, IInputStream* input, TVector<TVector<double>>* output) {
    TVector<double> line;
    for (ui32 i = 0; i < docCount; ++i) {
        ::Load(input, line);
        for (ui32 dim = 0; dim < output->size(); ++dim) {
            (*output)[dim][i] = line[dim];
        }
    }
}

static ui32 GetDocCount(TConstArrayRef<TProcessedDataProvider> datasetParts) {
    ui32 answer = 0;
    for (const auto& datasetPart : datasetParts) {
        answer += datasetPart.ObjectsData->GetObjectCount();
    }
    return answer;
}

static void InitApproxBuffer(
    int approxDimension,
    TConstArrayRef<TProcessedDataProvider> datasetParts,
    bool initBaselineIfAvailable,
    TVector<TVector<double>>* approxMatrix
) {
    approxMatrix->resize(approxDimension);
    if (datasetParts.empty())
        return;

    bool hasBaseline = false;
    if (initBaselineIfAvailable) {
        hasBaseline = !GetBaseline(datasetParts[0].TargetData).empty();
        for (auto datasetPartIdx : xrange<size_t>(1, datasetParts.size())) {
            CB_ENSURE(
                !GetBaseline(datasetParts[datasetPartIdx].TargetData).empty() == hasBaseline,
                "Inconsistent baseline specification between dataset parts: part 0 has "
                << (hasBaseline ? "" : "no ") << " baseline, but part " << datasetPartIdx << " has"
                << (hasBaseline ? " not" : "")
            );
        }
    }

    ui32 docCount = GetDocCount(datasetParts);

    for (auto approxIdx : xrange(approxDimension)) {
        auto& approx = (*approxMatrix)[approxIdx];
        if (hasBaseline) {
            approx.reserve(docCount);
            for (const auto& datasetPart : datasetParts) {
                auto baselinePart = GetBaseline(datasetPart.TargetData)[approxIdx];
                approx.insert(approx.end(), baselinePart.begin(), baselinePart.end());
            }
            Y_ASSERT(approx.size() == (size_t)docCount);
        } else {
            approx.resize(docCount);
        }
    }
}

static void ClearApproxBuffer(TVector<TVector<double>>* approxMatrix) {
    for (auto& approx : *approxMatrix) {
        approx.clear();
    }
}

TMetricsPlotCalcer& TMetricsPlotCalcer::ProceedDataSet(
    const TProcessedDataProvider& processedData,
    ui32 beginIterationIndex,
    ui32 endIterationIndex,
    bool isAdditiveMetrics
) {
    TModelCalcerOnPool modelCalcerOnPool(Model, processedData.ObjectsData, &Executor);

    const ui32 docCount = processedData.ObjectsData->GetObjectCount();
    InitApproxBuffer(
        Model.ObliviousTrees.ApproxDimension,
        MakeArrayRef(&processedData, 1),
        /*initBaselineIfAvailable*/ beginIterationIndex == 0,
        &CurApproxBuffer
    );

    ui32 begin, end;
    if (beginIterationIndex == 0) {
        begin = 0;
    } else {
        begin = Iterations[beginIterationIndex];
        Load(docCount, LastApproxes.Get(), &CurApproxBuffer);
    }

    const auto target = GetTarget(processedData.TargetData);
    const auto weights = GetWeights(processedData.TargetData);
    const auto groupInfos = GetGroupInfo(processedData.TargetData);

    for (ui32 iterationIndex = beginIterationIndex; iterationIndex < endIterationIndex; ++iterationIndex) {
        end = Iterations[iterationIndex] + 1;
        modelCalcerOnPool.ApplyModelMulti(EPredictionType::InternalRawFormulaVal, begin, end, &FlatApproxBuffer, &NextApproxBuffer);
        Append(NextApproxBuffer, &CurApproxBuffer);

        if (isAdditiveMetrics) {
            ComputeAdditiveMetric(
                CurApproxBuffer,
                target,
                weights,
                groupInfos,
                iterationIndex);
        } else {
            SaveApproxToFile(iterationIndex, CurApproxBuffer);
        }
        begin = end;
    }
    ClearApproxBuffer(&CurApproxBuffer);
    ClearApproxBuffer(&NextApproxBuffer);

    return *this;
}

void TMetricsPlotCalcer::ComputeNonAdditiveMetrics(ui32 begin, ui32 end) {
    const auto& target = NonAdditiveMetricsData.Target;
    const auto& weights = NonAdditiveMetricsData.Weights;
    for (ui32 idx = begin; idx < end; ++idx) {
        auto approx = LoadApprox(idx);
        for (ui32 metricId = 0; metricId < NonAdditiveMetrics.size(); ++metricId) {
            NonAdditiveMetricPlots[metricId][idx] = NonAdditiveMetrics[metricId]->Eval(approx, target, weights, {}, 0, target.size(), Executor);
        }
        if (idx != 0) {
            DeleteApprox(idx - 1);
        }
    }
}

static TVector<float> BuildTargets(const TVector<TProcessedDataProvider>& datasetParts) {
    TVector<float> result;
    result.reserve(GetDocCount(datasetParts));
    for (const auto& datasetPart : datasetParts) {
        const auto target = GetTarget(datasetPart.TargetData);
        result.insert(result.end(), target.begin(), target.end());
    }
    return result;
}

static TVector<float> BuildWeights(const TVector<TProcessedDataProvider>& datasetParts) {
    TVector<float> result;
    result.reserve(GetDocCount(datasetParts));
    for (const auto& datasetPart : datasetParts) {
        const auto weights = GetWeights(datasetPart.TargetData);
        result.insert(result.end(), weights.begin(), weights.end());
    }
    return result;
}

static TVector<ui32> GetStartDocIdx(const TVector<TProcessedDataProvider>& datasetParts) {
    TVector<ui32> result;
    result.reserve(datasetParts.size());
    ui32 start = 0;
    for (const auto& datasetPart : datasetParts) {
        result.push_back(start);
        start += datasetPart.ObjectsData->GetObjectCount();
    }
    return result;
}

void TMetricsPlotCalcer::ComputeNonAdditiveMetrics(const TVector<TProcessedDataProvider>& datasetParts) {
    TVector<float> allTargets = BuildTargets(datasetParts);
    TVector<float> allWeights = BuildWeights(datasetParts);


    TVector<TVector<double>> curApprox;
    InitApproxBuffer(
        Model.ObliviousTrees.ApproxDimension,
        datasetParts,
        /*initBaselineIfAvailable*/ true,
        &curApprox
    );

    int begin = 0;
    TVector<TModelCalcerOnPool> modelCalcers;
    for (const auto& datasetPart : datasetParts) {
        modelCalcers.emplace_back(Model, datasetPart.ObjectsData, &Executor);
    }

    auto startDocIdx = GetStartDocIdx(datasetParts);
    for (ui32 iterationIndex = 0; iterationIndex < Iterations.size(); ++iterationIndex) {
        int end = Iterations[iterationIndex] + 1;
        for (int poolPartIdx = 0; poolPartIdx < modelCalcers.ysize(); ++poolPartIdx) {
            auto& calcer = modelCalcers[poolPartIdx];
            calcer.ApplyModelMulti(EPredictionType::InternalRawFormulaVal, begin, end, &FlatApproxBuffer, &NextApproxBuffer);
            Append(NextApproxBuffer, &curApprox, startDocIdx[poolPartIdx]);
        }

        for (ui32 metricId = 0; metricId < NonAdditiveMetrics.size(); ++metricId) {
            NonAdditiveMetricPlots[metricId][iterationIndex] = NonAdditiveMetrics[metricId]->Eval(curApprox, allTargets, allWeights, {}, 0, allTargets.size(), Executor);
        }
        begin = end;
    }
}

TString TMetricsPlotCalcer::GetApproxFileName(ui32 plotLineIndex) {
    const ui32 plotSize = plotLineIndex + 1;
    if (NonAdditiveMetricsData.ApproxFiles.size() < plotSize) {
        NonAdditiveMetricsData.ApproxFiles.resize(plotSize);
    }
    if (NonAdditiveMetricsData.ApproxFiles[plotLineIndex].Empty()) {
        if (!NFs::Exists(TmpDir)) {
            NFs::MakeDirectory(TmpDir);
            DeleteTmpDirOnExitFlag = true;
        }
        TString name = TStringBuilder() << CreateGuidAsString() << "_approx_" << plotLineIndex << ".tmp";
        auto path = JoinFsPaths(TmpDir, name);
        if (NFs::Exists(path)) {
            CATBOOST_INFO_LOG << "Path already exists " << path << ". Will overwrite file" << Endl;
            NFs::Remove(path);
        }
        NonAdditiveMetricsData.ApproxFiles[plotLineIndex] = path;
    }
    return NonAdditiveMetricsData.ApproxFiles[plotLineIndex];
}

void TMetricsPlotCalcer::SaveApproxToFile(ui32 plotLineIndex,
                                          const TVector<TVector<double>>& approx) {
    auto fileName = GetApproxFileName(plotLineIndex);
    ui32 docCount = approx[0].size();
    TFile file(fileName, EOpenModeFlag::ForAppend | EOpenModeFlag::OpenAlways);
    TOFStream out(file);
    TVector<double> line(approx.size());

    for (ui32 i = 0; i < docCount; ++i) {
        for (ui32 dim = 0; dim < approx.size(); ++dim) {
            line[dim] = approx[dim][i];
        }
        ::Save(&out, line);
    }
}

TVector<TVector<double>> TMetricsPlotCalcer::LoadApprox(ui32 plotLineIndex) {
    TIFStream input(GetApproxFileName(plotLineIndex));
    ui32 docCount = NonAdditiveMetricsData.Target.size();
    TVector<TVector<double>> result(Model.ObliviousTrees.ApproxDimension, TVector<double>(docCount));
    Load(docCount, &input, &result);
    return result;
}

void TMetricsPlotCalcer::DeleteApprox(ui32 plotLineIndex) {
    NFs::Remove(GetApproxFileName(plotLineIndex));
}

static inline ELossFunction ReadLossFunction(const TString& modelInfoParams) {
    return ParseLossType(ReadTJsonValue(modelInfoParams)["loss_function"]["type"].GetStringSafe());
}

TMetricsPlotCalcer CreateMetricCalcer(
    const TFullModel& model,
    int begin,
    int end,
    int evalPeriod,
    int processedIterationsStep,
    NPar::TLocalExecutor& executor,
    const TString& tmpDir,
    const TVector<THolder<IMetric>>& metrics
) {
    if (model.ModelInfo.contains("params") && ReadTJsonValue(model.ModelInfo.at("params")).Has("loss_function")) {
        CheckMetrics(metrics, ReadLossFunction(model.ModelInfo.at("params")));
    }

    if (end == 0) {
        end = model.GetTreeCount();
    } else {
        end = Min<int>(end, model.GetTreeCount());
    }

    if (evalPeriod > (end - begin)) {
        evalPeriod = end - begin;
    }

    TMetricsPlotCalcer plotCalcer(model, metrics, executor, tmpDir, begin, end, evalPeriod, processedIterationsStep);

    return plotCalcer;
}

TVector<TVector<double>> TMetricsPlotCalcer::GetMetricsScore() {
    TVector<TVector<double>> metricsScore(AdditiveMetrics.size() + NonAdditiveMetrics.size(), TVector<double>(Iterations.size()));
    for (ui32 i = 0; i < Iterations.size(); ++i) {
        for (ui32 metricId = 0; metricId < AdditiveMetrics.size(); ++metricId) {
            metricsScore[AdditiveMetricsIndices[metricId]][i] = AdditiveMetrics[metricId]->GetFinalError(AdditiveMetricPlots[metricId][i]);
        }
        for (ui32 metricId = 0; metricId < NonAdditiveMetrics.size(); ++metricId) {
            metricsScore[NonAdditiveMetricsIndices[metricId]][i] = NonAdditiveMetrics[metricId]->GetFinalError(NonAdditiveMetricPlots[metricId][i]);
        }
    }
    return metricsScore;
}

static TLogger CreateLogger(
    const TString& token,
    const TString& resultDir,
    const TString& metricsFile,
    int iterationBegin,
    int iterationEnd,
    int iterationPeriod,
    bool saveMetrics,
    const TVector<const IMetric*>& metrics
) {
    TLogger logger(iterationBegin, iterationEnd - 1, iterationPeriod);
    if (saveMetrics) {
        logger.AddBackend(token, TIntrusivePtr<ILoggingBackend>(new TErrorFileLoggingBackend(JoinFsPaths(resultDir, metricsFile))));
    }
    logger.AddBackend(token, TIntrusivePtr<ILoggingBackend>(new TTensorBoardLoggingBackend(JoinFsPaths(resultDir, token))));

    const int iterationsCount = ceil((iterationEnd - iterationBegin) / (iterationPeriod + 0.0));
    auto metaJson = GetJsonMeta(iterationsCount, /*optionalExperimentName=*/"", metrics, /*learnSetNames=*/{}, {token}, ELaunchMode::Eval);
    logger.AddBackend(token, TIntrusivePtr<ILoggingBackend>(new TJsonLoggingBackend(JoinFsPaths(resultDir, "catboost_training.json"), metaJson)));
    return logger;
}

TMetricsPlotCalcer& TMetricsPlotCalcer::SaveResult(const TString& resultDir, const TString& metricsFile, bool saveMetrics, bool saveStats) {
    TFsPath trainDirPath(resultDir);
    if (!resultDir.empty() && !trainDirPath.Exists()) {
        trainDirPath.MkDirs();
    }

    if (saveStats) {
        TOFStream statsStream(JoinFsPaths(resultDir, "partial_stats.tsv"));
        const char sep = '\t';
        WriteHeaderForPartialStats(&statsStream, sep);
        WritePartialStats(&statsStream, sep);
    }


    TVector<const IMetric*> metrics(AdditiveMetrics.size() + NonAdditiveMetrics.size());
    for (ui32 metricId = 0; metricId < AdditiveMetrics.size(); ++metricId) {
        metrics[AdditiveMetricsIndices[metricId]] = AdditiveMetrics[metricId];
    }
    for (ui32 metricId = 0; metricId < NonAdditiveMetrics.size(); ++metricId) {
        metrics[NonAdditiveMetricsIndices[metricId]] = NonAdditiveMetrics[metricId];
    }

    TVector<TVector<double>> results = GetMetricsScore();

    const TString token = "eval_dataset";
    TLogger logger = CreateLogger(token, resultDir, metricsFile, First, Last, Step, saveMetrics, metrics);
    for (int iteration = 0; iteration < results[0].ysize(); ++iteration) {
        TOneInterationLogger oneIterLogger(logger);
        for (int metricIdx = 0; metricIdx < results.ysize(); ++metricIdx) {
            oneIterLogger.OutputMetric(token, TMetricEvalResult(metrics[metricIdx]->GetDescription(), results[metricIdx][iteration], false));
        }
    }
    return *this;
}
