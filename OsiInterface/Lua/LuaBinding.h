#pragma once

#include <GameDefinitions/Character.h>
#include <GameDefinitions/Item.h>
#include <GameDefinitions/Status.h>
#include <Lua/LuaHelpers.h>

#include <mutex>
#include <unordered_set>
#include <optional>

namespace dse::eoc
{
	struct AiGrid;
}

namespace dse::lua
{
	void PushExtFunction(lua_State * L, char const * func);
	void PushModFunction(lua_State* L, char const* mod, char const* func);

	template <class T>
	class ObjectProxy : public Userdata<ObjectProxy<T>>, public Indexable, public NewIndexable, public Pushable<PushPolicy::Unbind>
	{
	public:
		static char const * const MetatableName;

		ObjectProxy(T * obj)
			: obj_(obj), handle_()
		{}

		ObjectProxy(ObjectHandle handle)
			: obj_(nullptr), handle_(handle)
		{}

		void Unbind()
		{
			obj_ = nullptr;
		}

		int Index(lua_State * L);
		int NewIndex(lua_State * L);
		T* Get(lua_State* L);

		static T* CheckedGet(lua_State* L, int index)
		{
			auto self = CheckUserData(L, index);
			auto obj = self->Get(L);
			if (obj != nullptr) {
				return obj;
			} else {
				luaL_error(L, "No object bound to object proxy of type '%s'", MetatableName);
				return nullptr;
			}
		}

	protected:
		int GenericGetter(lua_State* L, PropertyMapBase const& propertyMap)
		{
			StackCheck _(L, 1);
			auto obj = Get(L);
			if (!obj) {
				push(L, nullptr);
				return 1;
			}

			auto prop = luaL_checkstring(L, 2);
			auto fetched = LuaPropertyMapGet(L, propertyMap, obj, prop, true);
			if (!fetched) {
				push(L, nullptr);
			}

			return 1;
		}

		int GenericSetter(lua_State* L, PropertyMapBase const& propertyMap)
		{
			StackCheck _(L, 0);
			auto obj = Get(L);
			if (!obj) return 0;

			auto prop = luaL_checkstring(L, 2);
			LuaPropertyMapSet(L, 3, propertyMap, obj, prop, true);

			return 0;
		}

	private:
		T * obj_;
		ObjectHandle handle_;
	};


	class StatsProxy : public Userdata<StatsProxy>, public Indexable, public NewIndexable, public Pushable<PushPolicy::Unbind>
	{
	public:
		static char const * const MetatableName;

		StatsProxy(CRPGStats_Object * obj, std::optional<int> level)
			: obj_(obj), level_(level)
		{}

		void Unbind()
		{
			obj_ = nullptr;
		}

		int Index(lua_State * L);
		int NewIndex(lua_State * L);

	private:
		CRPGStats_Object * obj_;
		std::optional<int> level_;
	};


	class SkillPrototypeProxy : public Userdata<SkillPrototypeProxy>, public Indexable, public Pushable<PushPolicy::Unbind>
	{
	public:
		static char const * const MetatableName;

		SkillPrototypeProxy(SkillPrototype * obj, std::optional<int> level);

		void Unbind()
		{
			obj_ = nullptr;
		}

		int Index(lua_State * L);

	private:
		SkillPrototype * obj_;
		CRPGStats_Object * stats_;
		std::optional<int> level_;
	};


	class ItemOrCharacterPushPin
	{
	public:
		ItemOrCharacterPushPin(lua_State * L, CRPGStats_Object * obj);
		~ItemOrCharacterPushPin();

	private:
		ObjectProxy<CDivinityStats_Character> * character_{ nullptr };
		ObjectProxy<CDivinityStats_Item> * item_{ nullptr };
		StatsProxy * object_{ nullptr };
	};


	class StatsExtraDataProxy : public Userdata<StatsExtraDataProxy>, public Indexable, 
		public NewIndexable, public Pushable<PushPolicy::None>
	{
	public:
		static char const * const MetatableName;

		int Index(lua_State * L);
		int NewIndex(lua_State * L);
	};


	class DamageList : public Userdata<DamageList>, public Pushable<PushPolicy::None>
	{
	public:
		static char const * const MetatableName;

		static void PopulateMetatable(lua_State * L);

		inline DamagePairList & Get()
		{
			return damages_;
		}

	private:
		DamagePairList damages_;

		static int GetByType(lua_State * L);
		static int Add(lua_State * L);
		static int Clear(lua_State * L);
		static int Multiply(lua_State * L);
		static int Merge(lua_State * L);
		static int ConvertDamageType(lua_State * L);
		static int AggregateSameTypeDamages(lua_State * L);
		static int ToTable(lua_State * L);
	};


	class ExtensionLibrary
	{
	public:
		virtual void Register(lua_State * L);
		virtual void RegisterLib(lua_State * L) = 0;

	protected:
		static int Include(lua_State * L);
	};

	class Exception : public std::runtime_error
	{
	public:
		Exception(std::string const& msg)
			: std::runtime_error("Lua error thrown: " + msg)
		{}
	};

	class State : Noncopyable<State>
	{
	public:
		enum RestrictionFlag
		{
			// Disable Osiris calls
			RestrictOsiris = 1 << 0,
			// Disable handle/guid to object conversion functions (Lua only)
			RestrictHandleConversion = 1 << 1,

			// Permit calls only available during module load state
			ScopeModuleLoad = 1 << 16,
			// Permit calls only available during session load state
			ScopeSessionLoad = 1 << 17,
			// Permit calls only available during module resume state
			ScopeModuleResume = 1 << 18,
			// Permit calls only available during initial module load state (before loading any files)
			ScopeModulePreLoad = 1 << 19,

			RestrictAll = 0x0000ffff,
		};

		uint32_t RestrictionFlags{ 0 };
		std::unordered_set<int32_t> OverriddenLevelMaps;

		State();
		~State();

		State(State const &) = delete;
		State(State &&) = delete;
		State & operator = (State const &) = delete;
		State & operator = (State &&) = delete;

		inline lua_State * GetState()
		{
			return L;
		}

		inline bool StartupDone() const
		{
			return startupDone_;
		}

		void FinishStartup();
		void LoadBootstrap(STDString const& path, STDString const& modTable);
		virtual void OnGameSessionLoading();
		void OnGameSessionLoaded();
		void OnModuleLoadStarted();
		void OnModuleLoading();
		void OnStatsLoaded();
		void OnModuleResume();

		template <class... Ret, class... Args>
		auto CallExt(char const * func, uint32_t restrictions, ReturnType<Ret...>, Args... args)
		{
			Restriction restriction(*this, restrictions);
			PushExtFunction(L, func);
			auto _{ PushArguments(L, std::tuple{args...}) };
			return CheckedCall<Ret...>(L, sizeof...(args), func);
		}

		std::optional<int> LoadScript(STDString const & script, STDString const & name = "", int globalsIdx = 0);

		std::optional<int32_t> GetHitChance(CDivinityStats_Character * attacker, CDivinityStats_Character * target);
		bool GetSkillDamage(SkillPrototype * self, DamagePairList * damageList,
			CRPGStats_ObjectInstance *attackerStats, bool isFromItem, bool stealthed, float * attackerPosition,
			float * targetPosition, DeathType * pDeathType, int level, bool noRandomization);
		std::optional<std::pair<int, bool>> GetSkillAPCost(SkillPrototype* skill, CDivinityStats_Character* character, eoc::AiGrid* aiGrid,
			glm::vec3* position, float* radius);
		void OnNetMessageReceived(STDString const & channel, STDString const & payload, UserId userId);

		static STDString GetBuiltinLibrary(int resourceId);

	protected:
		lua_State * L;
		bool startupDone_{ false };

		void OpenLibs();
	};

	class Restriction
	{
	public:
		inline Restriction(State & state, uint32_t flags)
			: state_(state), oldFlags_(state_.RestrictionFlags)
		{
			state_.RestrictionFlags |= flags;
		}

		inline ~Restriction()
		{
			state_.RestrictionFlags = oldFlags_;
		}

	private:
		State & state_;
		uint32_t oldFlags_;
	};

	void RegisterStatsObjects(lua_State* L);

	int GetExtensionVersionWrapper(lua_State* L);
	int GetGameVersionWrapper(lua_State* L);
	int MonotonicTimeWrapper(lua_State* L);
	int OsiPrint(lua_State* L);
	int OsiPrintWarning(lua_State* L);
	int OsiPrintError(lua_State* L);
	int HandleToDoubleWrapper(lua_State* L);
	int DoubleToHandleWrapper(lua_State* L);
	int GetHandleTypeWrapper(lua_State* L);
	int GetCellInfo(lua_State* L);

	int SaveFileWrapper(lua_State* L);
	int LoadFileWrapper(lua_State* L);

	int JsonParse(lua_State* L);
	int JsonStringify(lua_State* L);

	int IsModLoadedWrapper(lua_State* L);
	int GetModLoadOrder(lua_State* L);
	int GetModInfo(lua_State* L);
	int LuaDebugBreakWrapper(lua_State* L);

	int GetStatEntries(lua_State* L);
	int GetStatEntriesLoadedBefore(lua_State* L);
	int GetSkillSet(lua_State* L);
	int UpdateSkillSet(lua_State* L);
	int GetEquipmentSet(lua_State* L);
	int UpdateEquipmentSet(lua_State* L);
	int GetTreasureTable(lua_State* L);
	int UpdateTreasureTable(lua_State* L);
	int GetTreasureCategory(lua_State* L);
	int UpdateTreasureCategory(lua_State* L);
	int GetItemCombo(lua_State* L);
	int UpdateItemCombo(lua_State* L);
	int GetItemComboPreviewData(lua_State* L);
	int UpdateItemComboPreviewData(lua_State* L);
	int GetItemComboProperty(lua_State* L);
	int UpdateItemComboProperty(lua_State* L);
	int GetItemGroup(lua_State* L);
	int GetNameGroup(lua_State* L);

	int StatGetAttribute(lua_State* L);
	int StatSetAttribute(lua_State* L);
	int StatAddCustomDescriptionWrapper(lua_State* L);
	int StatSetLevelScaling(lua_State* L);
	int GetStat(lua_State* L);
	int CreateStat(lua_State* L);
	int SyncStatWrapper(lua_State* L);
	int StatSetPersistenceWrapper(lua_State* L);
	int GetDeltaMod(lua_State* L);
	int UpdateDeltaMod(lua_State* L);

	int EnumIndexToLabel(lua_State* L);
	int EnumLabelToIndex(lua_State* L);
	int NewDamageList(lua_State* L);
	int GetSurfaceTemplate(lua_State* L);

	int IsDeveloperModeWrapper(lua_State* L);
	int GetDifficultyWrapper(lua_State* L);
	int GetGameModeWrapper(lua_State* L);
	int AddPathOverrideWrapper(lua_State* L);
	int GetPathOverrideWrapper(lua_State* L);
	int LuaRandom(lua_State* L);
	int LuaRoundWrapper(lua_State* L);
	int AddVoiceMetaDataWrapper(lua_State* L);
	int GetTranslatedStringWrapper(lua_State* L);
	int GetTranslatedStringFromKeyWrapper(lua_State* L);
	int CreateTranslatedStringKeyWrapper(lua_State* L);
	int CreateTranslatedStringHandleWrapper(lua_State* L);
	int CreateTranslatedStringWrapper(lua_State* L);
	int GenerateIdeHelpersWrapper(lua_State* L);

	int EnableExperimentalPropertyWritesWrapper(lua_State* L);
	int DumpStackWrapper(lua_State* L);
	int ShowErrorAndExitGameWrapper(lua_State* L);
}
