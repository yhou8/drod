#ifndef PLAYERSTATS_H
#define PLAYERSTATS_H

#include "DbPackedVars.h"
#include <BackEndLib/Wchar.h>
#include <map>
#include <set>
#include <string>
using std::map;
using std::set;
using std::string;

//Script variable manipulation.
//
//To maintain backwards compatibility, don't alter the enumeration values.
namespace ScriptVars
{
	enum Op
	{
		Assign=0,
		Inc=1,
		Dec=2,
		AssignText=3,
		AppendText=4,
		MultiplyBy=5,
		DivideBy=6,
		Mod=7
	};
	enum Comp
	{
		Equals=0,
		Greater=1,
		Less=2,
		EqualsText=3,
		LessThanOrEqual=8, //to avoid collision with Op values
		GreaterThanOrEqual=9,
		Inequal=10
	};

	//Predefined global and relative game state vars, accessed through these key values.
	enum Predefined
	{
		P_NoVar  =  0,
		P_MONSTER_WEAPON = -1,
		//P_SWORD = -2,
		//P_MONSTER_COLOR = -3,
		P_PLAYER_X = -4,
		P_PLAYER_Y = -5,
		P_PLAYER_O = -6,
		P_MONSTER_X = -7,
		P_MONSTER_Y = -8,
		P_MONSTER_O = -9,
		//P_TOTALMOVES = -10,
		//P_TOTALTIME = -11,
		P_SCRIPT_X = -12,
		P_SCRIPT_Y = -13,
		P_SCRIPT_W = -14,
		P_SCRIPT_H = -15,
		P_SCRIPT_F = -16,
		P_ROOMIMAGE_X = -17,
		P_ROOMIMAGE_Y = -18,
		P_OVERHEADIMAGE_X = -19,
		P_OVERHEADIMAGE_Y = -20,
		P_LEVELNAME = -21,
		P_THREATCLOCK = -22,
		P_PLAYERLIGHT = -23,
		P_PLAYERLIGHTTYPE = -24,
		P_RETURN_X = -25,
		P_RETURN_Y = -26,
		FirstPredefinedVar = P_RETURN_Y, //set this to the last var in the enumeration
		PredefinedVarCount = -int(FirstPredefinedVar)
	};

	void init();
	string getVarName(const ScriptVars::Predefined var);
	WSTRING getVarNameW(const ScriptVars::Predefined var);
	bool IsStringVar(Predefined val);
	Predefined parsePredefinedVar(const string& str);
	Predefined parsePredefinedVar(const WSTRING& wstr);

	bool IsCharacterLocalVar(const WSTRING& wstr);
	bool IsCharacterLocalVar(const WCHAR* wstr);

	//All predefined vars.
	extern const UINT predefinedVarMIDs[PredefinedVarCount];
	extern string midTexts[PredefinedVarCount];
};

//Stats used for various tally operations.
class RoomStats
{
public:
	RoomStats() {clear();}
	virtual ~RoomStats() {}

	virtual void clear() {
		yellowDoors = greenDoors = blueDoors = redDoors = blackDoors = 0;
		openYellowDoors = openGreenDoors = openBlueDoors = openRedDoors = openBlackDoors = 0;
		rooms = secrets = levels = 0;
	}

	UINT yellowDoors, greenDoors, blueDoors, redDoors, blackDoors;
	UINT openYellowDoors, openGreenDoors, openBlueDoors, openRedDoors, openBlackDoors;
	UINT rooms, secrets, levels;
};

class CCueEvents;
struct Challenges
{
	Challenges() { }
	Challenges(CDbPackedVars& vars) { deserialize(vars); }

	void clear() { challenges.clear(); }
	void deserialize(CDbPackedVars& vars);
	void serialize(CDbPackedVars& vars) const;

	set<UINT> getHoldIDs() const;
	set<WSTRING> get(const UINT holdID) const;
	void rekeyHoldIDs(const map<UINT,UINT>& holdIDmap);

	bool add(const UINT holdID, CCueEvents* CueEvents);
	bool add(const UINT holdID, const set<WSTRING>& names);
	Challenges& operator+=(const Challenges& rhs);

	static void GetFrom(CCueEvents* CueEvents, set<WSTRING>& challengesCompleted);

private:
	typedef set<WSTRING> HoldChallenges;
	typedef map<UINT, HoldChallenges> ChallengeT;
	ChallengeT challenges; 
};

#endif
