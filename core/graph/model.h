#ifndef CORE_GRAPH_MODEL_H
#define CORE_GRAPH_MODEL_H

#include "core/graph/graph.h"

namespace LotusIR
{
    // A machine learning model representation class.
    // Besides a main <Graph>, it also holds basic information, say,
    // version, domain, model author, license etc.
    class Model
    {
    public:

        const VERSION c_noVersion = INT64_MAX;

        // <p_isONNX> is a special flag to indicate whether it's
        // going to construct a ONNX graph. With ONNX graph, strict
        // type checking will be skiped.
        Model(const std::string& p_graphName, bool p_isONNX = false);

        Model(const std::string& p_graphName,
            const std::string& p_graphDocString);

        Model(const std::string& p_graphName,
            const std::string& p_graphDocString,
            VERSION p_irVersion,
            const std::string& p_producerName,
            const std::string& p_producerVersion,
            const std::string& p_domain,
            VERSION p_modelVersion,
            const std::string& p_modelDocString,
            const std::string& p_modelAuthor,
            const std::string& p_modelLicense);

        Model(const ModelProto& p_modelProto);

        // Get model's IR version.
        // Return <c_noVersion> if not specified.
        VERSION IrVersion() const;
        // Set model's IR version.
        void SetIrVersion(VERSION p_irVersion);

        // Get model's producer name.
        // Return null pointer if not specified.
        const std::string& ProducerName() const;
        // Set model's producer name.
        void SetProducerName(const std::string& p_producerName);

        // Get model's producer version.
        // Return null pointer if not specified.
        const std::string& ProducerVersion() const;
        // Set model's producer version.
        void SetProducerVersion(const std::string& p_producerVersion);

        // Get model's domain.
        // Return null pointer if not specified.
        const std::string& Domain() const;
        // Set models' damain.
        void SetDomain(const std::string& p_domain);

        // Get model's version.
        // Return null pointer if not specified.
        VERSION ModelVersion() const;
        // Set models' version.
        void SetModelversion(VERSION p_modelVersion);

        // Get model's doc string.
        // Return null pointer if not specified.
        const std::string& DocString() const;
        // Set models' doc string.
        void SetDocString(const std::string& p_docString);

        // Get model's author.
        // Return null pointer if not specified.
        const std::string& ModelAuthor() const;
        // Set models' author.
        void SetModelAuthor(const std::string& p_modelAuthor);

        // Get model's license.
        // Return null pointer if not specified.
        const std::string& ModelLicense() const;
        // Set models' license.
        void SetModelLicense(const std::string& p_modelLicense);

        // Get model's main graph.
        // The return pointer is owned by <*this> model.
        Graph* MainGraph();

        // Get model's serlization proto data.
        const ModelProto& ToProto();

#ifdef _WIN32
        static Status Save(Model& p_model, const std::wstring& p_filePath);

        static Status Load(const std::wstring& p_filePath, /*out*/ std::shared_ptr<Model>* p_model);
#endif
        static Status Save(Model& p_model, const std::string& p_filePath);

        static Status Save(Model& p_model, int p_fd);

        static Status Load(const std::string& p_filePath, /*out*/ std::shared_ptr<Model>* p_model);

        static Status Load(int p_fd, /*out*/ std::shared_ptr<Model>* p_model);

    private:

        // Model data.
        ModelProto m_modelProto;

        // Main graph of the model.
        std::unique_ptr<Graph> m_graph;
    };
}

#endif