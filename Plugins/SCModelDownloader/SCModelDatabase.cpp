#include <metahook.h>
#include "plugins.h"

#include "UtilHTTPClient.h"
#include "UtilAssetsIntegrity.h"
#include "SCModelDatabase.h"

#include <string>
#include <unordered_map>
#include <algorithm>
#include <format>

#include <rapidjson/document.h>

bool SCModel_ShouldDownloadLatest();
void SCModel_ReloadModel(const char* name);

static unsigned int g_uiAllocatedTaskId = 0;

typedef struct scmodel_s
{
	int size;
	int flags;
	int polys;
}scmodel_t;

static std::unordered_map<std::string, scmodel_t> g_Database;

int SCModel_Hash(const std::string& name)
{
	int hash = 0;

	for (size_t i = 0; i < name.length(); i++) {
		char ch = (char)name[i];
		hash = ((hash << 5) - hash) + ch;
		hash = hash % 15485863; // prevent hash ever increasing beyond 31 bits
	}
	return hash;
}

class CUtilHTTPCallbacks : public IUtilHTTPCallbacks
{
private:
	ISCModelQuery* m_pQueryTask{};

public:
	CUtilHTTPCallbacks(ISCModelQuery* p) : m_pQueryTask(p)
	{

	}

	void Destroy() override
	{
		delete this;
	}

	void OnResponseComplete(IUtilHTTPRequest* RequestInstance, IUtilHTTPResponse* ResponseInstance) override
	{
		if (!RequestInstance->IsRequestSuccessful())
		{
			m_pQueryTask->OnFailure();
			return;
		}

		if (ResponseInstance->IsResponseError())
		{
			m_pQueryTask->OnFailure();
			return;
		}

		auto pPayload = ResponseInstance->GetPayload();

		if (!m_pQueryTask->OnProcessPayload((const char*)pPayload->GetBytes(), pPayload->GetLength()))
		{
			m_pQueryTask->OnFailure();
			return;
		}
	}

	void OnUpdateState(UtilHTTPRequestState NewState) override
	{
		if (NewState == UtilHTTPRequestState::Finished)
		{
			m_pQueryTask->OnFinish();
		}
	}

	void OnReceiveData(IUtilHTTPRequest* RequestInstance, IUtilHTTPResponse* ResponseInstance, const void* pData, size_t cbSize) override
	{
		//Only stream request has OnReceiveData
	}
};

class CSCModelQueryBase : public ISCModelQuery
{
private:
	bool m_bFinished{};
	bool m_bFailed{};
	float m_flNextRetryTime{};
	unsigned int m_uiTaskId{};

protected:
	std::string m_Url;
	UtilHTTPRequestId_t m_RequestId{ UTILHTTP_REQUEST_INVALID_ID };

public:
	CSCModelQueryBase()
	{
		m_uiTaskId = g_uiAllocatedTaskId;
		g_uiAllocatedTaskId++;
	}

	~CSCModelQueryBase()
	{
		if (m_RequestId != UTILHTTP_REQUEST_INVALID_ID)
		{
			UtilHTTPClient()->DestroyRequestById(m_RequestId);
			m_RequestId = UTILHTTP_REQUEST_INVALID_ID;
		}
	}

	const char* GetUrl() const override
	{
		return m_Url.c_str();
	}

	bool IsFinished() const override
	{
		return m_bFinished;
	}

	bool IsFailed() const override
	{
		return m_bFailed;
	}

	SCModelQueryState GetState() const override
	{
		if (m_bFailed)
			return SCModelQueryState_Failed;

		if (m_bFinished)
			return SCModelQueryState_Finished;

		return SCModelQueryState_Querying;
	}

	unsigned int GetTaskId() const override
	{
		return m_uiTaskId;
	}

	void OnFailure() override
	{
		m_bFailed = true;
		m_flNextRetryTime = (float)gEngfuncs.GetAbsoluteTime() + 5.0f;

		SCModelDatabase()->DispatchQueryStateChangeCallback(this, GetState());
	}

	void OnFinish() override
	{
		m_bFinished = true;

		SCModelDatabase()->DispatchQueryStateChangeCallback(this, GetState());
	}

	void RunFrame(float flCurrentAbsTime) override
	{
		if (IsFailed() && flCurrentAbsTime > m_flNextRetryTime)
		{
			StartQuery();
		}
	}

	virtual void StartQuery()
	{
		if (m_RequestId != UTILHTTP_REQUEST_INVALID_ID)
		{
			UtilHTTPClient()->DestroyRequestById(m_RequestId);
			m_RequestId = UTILHTTP_REQUEST_INVALID_ID;
		}

		m_bFailed = false;
		m_bFinished = false;
		SCModelDatabase()->DispatchQueryStateChangeCallback(this, GetState());
	}
};

class CSCModelQueryTaskList;

class CSCModelQueryModelFileTask : public CSCModelQueryBase
{
public:
	std::string m_localFileName;
	std::string m_networkFileName;
	CSCModelQueryTaskList* m_pQueryTaskList;

public:
	CSCModelQueryModelFileTask(CSCModelQueryTaskList* parent, const std::string& localFileName, const std::string& networkFileName) :
		CSCModelQueryBase(),
		m_pQueryTaskList(parent),
		m_localFileName(localFileName),
		m_networkFileName(networkFileName)
	{

	}

	void StartQuery() override;

	bool OnProcessPayload(const char* data, size_t size) override;

	const char* GetName() const override
	{
		return "QueryModelFileTask";
	}

	const char* GetIdentifier() const override
	{
		return m_localFileName.c_str();
	}
};

class CSCModelQueryTaskList : public CSCModelQueryBase
{
public:
	int m_repoId{};
	std::string m_lowerName;
	std::string m_localFileNameBase;
	std::string m_networkFileNameBase;
	bool m_bHasTModel{};
	bool m_bReloaded{};

	std::vector<std::shared_ptr<ISCModelQuery>> m_SubQueryList;

public:
	CSCModelQueryTaskList(const std::string& lowerName, const std::string& localFileNameBase) :
		CSCModelQueryBase(),
		m_repoId(SCModel_Hash(lowerName) % 32),
		m_lowerName(lowerName), 
		m_localFileNameBase(localFileNameBase)
	{

	}

	bool IsFinished() const override
	{
		if (!CSCModelQueryBase::IsFinished())
			return false;

		for (auto &itor : m_SubQueryList)
		{
			if (!itor->IsFinished())
				return false;
		}

		return true;
	}

	bool IsAllRequiredFilesPresent()
	{
		std::string mdl = std::format("models/player/{0}/{0}.mdl", m_localFileNameBase);

		if (!FILESYSTEM_ANY_FILEEXISTS(mdl.c_str()))
			return false;

		if (m_bHasTModel)
		{
			std::string Tmdl = std::format("models/player/{0}/{0}T.mdl", m_localFileNameBase);
			std::string tmdl = std::format("models/player/{0}/{0}t.mdl", m_localFileNameBase);

			//Linux is unhappy with t.mdl
			if (!FILESYSTEM_ANY_FILEEXISTS(Tmdl.c_str()) && !FILESYSTEM_ANY_FILEEXISTS(tmdl.c_str()))
				return false;
		}

		return true;
	}

	void StartQuery() override
	{
		CSCModelQueryBase::StartQuery();

		m_Url = std::format("https://wootdata.github.io/scmodels_data_{0}/models/player/{1}/{1}.json", m_repoId, m_lowerName);

		auto pRequestInstance = UtilHTTPClient()->CreateAsyncRequest(m_Url.c_str(), UtilHTTPMethod::Get, new CUtilHTTPCallbacks(this));
		
		if (!pRequestInstance)
		{
			CSCModelQueryBase::OnFailure();
			return;
		}

		UtilHTTPClient()->AddToRequestPool(pRequestInstance);

		m_RequestId = pRequestInstance->GetRequestId();

		pRequestInstance->Send();
	}

	void BuildQueryInternal(const std::string& localFileName, const std::string& networkFileName)
	{
		if (std::find_if(m_SubQueryList.begin(), m_SubQueryList.end(), [&networkFileName](const std::shared_ptr<ISCModelQuery>& p) {

			std::string url = p->GetUrl();

			if (url.ends_with(networkFileName))
			{
				return true;
			}

			return false;

		}) == m_SubQueryList.end())
		{
			auto QueryInstance = std::make_shared<CSCModelQueryModelFileTask>(this, localFileName, networkFileName);

			QueryInstance->StartQuery();

			m_SubQueryList.emplace_back(QueryInstance);
		}
	}

	bool OnProcessPayload(const char* data, size_t size) override
	{
		rapidjson::Document doc;

		doc.Parse(data, size);

		if (doc.HasParseError())
		{
			gEngfuncs.Con_DPrintf("[SCModelDownloader] Failed to parse model json response!\n");
			return false;
		}

		auto obj = doc.GetObj();

		bool bHasTModel = false;

		std::string networkFileNameBase = m_lowerName;

		auto json_t_model = doc.FindMember("t_model");
		if (json_t_model != doc.MemberEnd() && json_t_model->value.IsBool())
		{
			bHasTModel = json_t_model->value.GetBool();
		}

		auto json_name = doc.FindMember("name");
		if (json_name != doc.MemberEnd() && json_name->value.IsString())
		{
			std::string name = json_name->value.GetString();

			if (name.size() > 4 &&
				name[name.size() - 4] == '.' &&
				name[name.size() - 3] == 'm' &&
				name[name.size() - 2] == 'd' &&
				name[name.size() - 1] == 'l')
			{
				name = name.erase(name.size() - 4, 4);
			}

			if (0 == stricmp(networkFileNameBase.c_str(), name.c_str()))
			{
				networkFileNameBase = name;
			}
		}

		m_networkFileNameBase = networkFileNameBase;
		m_bHasTModel = bHasTModel;

		gEngfuncs.Con_DPrintf("[SCModelDownloader] Json for model \"%s\" acquired!\n", m_localFileNameBase.c_str());

		if (!m_networkFileNameBase.empty())
		{
			BuildQueryInternal(m_localFileNameBase + ".mdl", m_networkFileNameBase + ".mdl");
		}

		if (!m_networkFileNameBase.empty() && m_bHasTModel)
		{
			BuildQueryInternal(m_localFileNameBase + "T.mdl", m_networkFileNameBase + "t.mdl");
			BuildQueryInternal(m_localFileNameBase + "T.mdl", m_networkFileNameBase + "T.mdl");
		}

		if (!m_networkFileNameBase.empty())
		{
			BuildQueryInternal(m_localFileNameBase + ".bmp", m_networkFileNameBase + ".bmp");
		}

		return true;
	}

	void OnModelFileWriteFinished()
	{
		if (!m_bReloaded && IsAllRequiredFilesPresent())
		{
			SCModel_ReloadModel(m_localFileNameBase.c_str());
		}
	}

	const char* GetName() const override
	{
		return "QueryTaskList";
	}

	const char* GetIdentifier() const override
	{
		return m_lowerName.c_str();
	}
};

void CSCModelQueryModelFileTask::StartQuery()
{
	CSCModelQueryBase::StartQuery();

	m_Url = std::format("https://wootdata.github.io/scmodels_data_{0}/models/player/{1}/{2}", m_pQueryTaskList->m_repoId, m_pQueryTaskList->m_networkFileNameBase, m_networkFileName);

	auto pRequestInstance = UtilHTTPClient()->CreateAsyncRequest(m_Url.c_str(), UtilHTTPMethod::Get, new CUtilHTTPCallbacks(this));

	if (!pRequestInstance)
	{
		CSCModelQueryBase::OnFailure();
		return;
	}

	UtilHTTPClient()->AddToRequestPool(pRequestInstance);

	m_RequestId = pRequestInstance->GetRequestId();

	pRequestInstance->Send();

}

bool CSCModelQueryModelFileTask::OnProcessPayload(const char* data, size_t size)
{
	gEngfuncs.Con_DPrintf("[SCModelDownloader] File \"%s\" acquired!\n", m_localFileName.c_str());

	if (m_localFileName.ends_with(".mdl"))
	{
		if (UtilAssetsIntegrityCheckReason::OK != UtilAssetsIntegrity()->CheckStudioModel(data, size, NULL))
		{
			gEngfuncs.Con_DPrintf("[SCModelDownloader] File \"%s\" is corrupted!\n", m_localFileName.c_str());
			return false;
		}
	}
	else if (m_localFileName.ends_with(".bmp"))
	{
		if (UtilAssetsIntegrityCheckReason::OK != UtilAssetsIntegrity()->Check8bitBMP(data, size, NULL))
		{
			gEngfuncs.Con_DPrintf("[SCModelDownloader] File \"%s\" is corrupted!\n", m_localFileName.c_str());
			return false;
		}
	}

	FILESYSTEM_ANY_CREATEDIR("models", "GAMEDOWNLOAD");
	FILESYSTEM_ANY_CREATEDIR("models/player", "GAMEDOWNLOAD");

	std::string filePathDir = std::format("models/player/{0}", m_pQueryTaskList->m_localFileNameBase);
	FILESYSTEM_ANY_CREATEDIR(filePathDir.c_str(), "GAMEDOWNLOAD");

	std::string filePath = std::format("models/player/{0}/{1}", m_pQueryTaskList->m_localFileNameBase, m_localFileName);
	auto FileHandle = FILESYSTEM_ANY_OPEN(filePath.c_str(), "wb", "GAMEDOWNLOAD");

	if (FileHandle)
	{
		FILESYSTEM_ANY_WRITE(data, size, FileHandle);
		FILESYSTEM_ANY_CLOSE(FileHandle);
	}

	m_pQueryTaskList->OnModelFileWriteFinished();

	return true;
}

class CSCModelQueryDatabase : public CSCModelQueryBase
{
public:
	CSCModelQueryDatabase() : CSCModelQueryBase()
	{

	}

	const char* GetName() const override
	{
		return "QueryDatabase";
	}

	const char* GetIdentifier() const override
	{
		return "";
	}

	void StartQuery() override
	{
		CSCModelQueryBase::StartQuery();

		m_Url = "https://raw.githubusercontent.com/wootguy/scmodels/master/database/models.json";

		auto pRequestInstance = UtilHTTPClient()->CreateAsyncRequest(m_Url.c_str(), UtilHTTPMethod::Get, new CUtilHTTPCallbacks(this));

		if (!pRequestInstance)
		{
			CSCModelQueryBase::OnFailure();
			return;
		}

		UtilHTTPClient()->AddToRequestPool(pRequestInstance);

		m_RequestId = pRequestInstance->GetRequestId();

		pRequestInstance->Send();
	}

	bool OnProcessPayload(const char* data, size_t size) override
	{
		if (g_Database.size() > 0)
		{
			gEngfuncs.Con_DPrintf("[SCModelDownloader] Database already filled!\n");
			return true;
		}

		rapidjson::Document doc;

		doc.Parse(data, size);

		if (doc.HasParseError())
		{
			gEngfuncs.Con_DPrintf("[SCModelDownloader] Failed to parse database response!\n");
			return false;
		}

		for (auto itor = doc.MemberBegin(); itor != doc.MemberEnd(); ++itor)
		{
			std::string name = itor->name.GetString();

			const auto &obj = itor->value.GetObj();

			scmodel_t m = { 0 };

			const auto &m_size = obj.FindMember("size");
			if (m_size != obj.MemberEnd() && m_size->value.IsInt())
			{
				m.size = m_size->value.GetInt();
			}

			const auto& m_flags = obj.FindMember("flags");
			if (m_flags != obj.MemberEnd() && m_flags->value.IsInt())
			{
				m.flags = m_flags->value.GetInt();
			}

			const auto& m_polys = obj.FindMember("polys");
			if (m_polys != obj.MemberEnd() && m_polys->value.IsInt())
			{
				m.polys = m_polys->value.GetInt();
			}

			g_Database[name] = m;
		}

		return true;
	}
};

class CSCModelDatabase : public ISCModelDatabase
{
private:
	std::vector<std::shared_ptr<ISCModelQuery>> m_QueryTaskList;
	std::vector<ISCModelQueryStateChangeHandler*> m_StateChangeCallbacks;

public:

	void Init() override
	{
		if (!g_Database.empty())
			return;

		if (!m_QueryTaskList.empty())
			return;

		auto pDatabaseQuery = std::make_shared<CSCModelQueryDatabase>();

		gEngfuncs.Con_DPrintf("[SCModelDownloader] Querying database...\n");

		m_QueryTaskList.emplace_back(pDatabaseQuery);

		pDatabaseQuery->StartQuery();
	}

	void Shutdown() override
	{
		m_QueryTaskList.clear();
		m_StateChangeCallbacks.clear();
		g_Database.clear();
	}

	void RunFrame() override
	{
		auto flCurrentAbsTime = (float)gEngfuncs.GetAbsoluteTime();

		for (auto itor = m_QueryTaskList.begin(); itor != m_QueryTaskList.end();)
		{
			const auto& p = (*itor);

			p->RunFrame(flCurrentAbsTime);

			if (p->IsFinished())
			{
				itor = m_QueryTaskList.erase(itor);
				continue;
			}

			itor++;
		}
	}

	/*
		Purpose: Build network query instance
	*/

	bool BuildQueryListInternal(const std::string& lowerName, const std::string& localFileNameBase)
	{
		for (const auto& p : m_QueryTaskList)
		{
			if (!strcmp(p->GetName(), "QueryTaskList") &&
				!strcmp(p->GetIdentifier(), lowerName.c_str()) )
			{
				return false;
			}
		}

		auto QueryList = std::make_shared<CSCModelQueryTaskList>(lowerName, localFileNameBase);

		m_QueryTaskList.emplace_back(QueryList);

		QueryList->StartQuery();

		return true;
	}

	/*
		Purpose: Build network query instance based on modelName
		lowerName is always lower-case, while modelName is not
	*/

	bool BuildQueryList(const std::string &modelName)
	{
		std::string lowerName = modelName;

		std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);

		//Model name ends with "_v0" ~ "_v9"
		if (lowerName.length() > 4 &&
			lowerName[lowerName.length() - 3] == '_' &&
			lowerName[lowerName.length() - 2] == 'v' &&
			lowerName[lowerName.length() - 1] >= '0' &&
			lowerName[lowerName.length() - 1] <= '9')
		{
			//has version?
			auto itor = g_Database.find(lowerName);
			if (itor != g_Database.end())
			{
				return BuildQueryListInternal(lowerName, modelName);
			}
			else
			{
				gEngfuncs.Con_DPrintf("[SCModelDownloader] \"%s\" not found in database.\n", modelName.c_str());
				return false;
			}
		}

		//Try latest if no version was found
		if (SCModel_ShouldDownloadLatest())
		{
			for (int i = 9; i >= 1; --i)
			{
				auto lowerName2 = lowerName;
				lowerName2 += "_v";
				lowerName2 += ('0' + i);

				auto itor = g_Database.find(lowerName2);

				if (itor != g_Database.end())
				{
					return BuildQueryListInternal(lowerName2, modelName);
				}
			}
		}

		return BuildQueryListInternal(lowerName, modelName);;
	}

	void OnMissingModel(const char* name) override
	{
		gEngfuncs.Con_DPrintf("[SCModelDownloader] Missing model \"%s\"...\n", name);

		BuildQueryList(name);
	}

	void EnumQueries(IEnumSCModelQueryHandler* handler) override
	{
		for (const auto& p : m_QueryTaskList)
		{
			handler->OnEnumQuery(p.get());
		}
	}

	void RegisterQueryStateChangeCallback(ISCModelQueryStateChangeHandler* handler) override
	{
		auto itor = std::find_if(m_StateChangeCallbacks.begin(), m_StateChangeCallbacks.end(), [handler](ISCModelQueryStateChangeHandler* it) {
			return it == handler;
		});

		if (itor == m_StateChangeCallbacks.end())
		{
			m_StateChangeCallbacks.emplace_back(handler);
		}
	}

	void UnregisterQueryStateChangeCallback(ISCModelQueryStateChangeHandler* handler) override
	{
		auto itor = std::find_if(m_StateChangeCallbacks.begin(), m_StateChangeCallbacks.end(), [handler](ISCModelQueryStateChangeHandler* it) {
			return it == handler;
			});

		if (itor != m_StateChangeCallbacks.end())
		{
			m_StateChangeCallbacks.erase(itor);
		}
	}

	void DispatchQueryStateChangeCallback(ISCModelQuery* pQuery, SCModelQueryState newState) override
	{
		for (auto callback : m_StateChangeCallbacks)
		{
			callback->OnQueryStateChanged(pQuery, newState);
		}
	}
};

static CSCModelDatabase s_SCModelDatabase;

ISCModelDatabase* SCModelDatabase()
{
	return &s_SCModelDatabase;
}

EXPOSE_SINGLE_INTERFACE_GLOBALVAR(CSCModelDatabase, ISCModelDatabase, SCMODEL_DATABASE_INTERFACE_VERSION, s_SCModelDatabase)