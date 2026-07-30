/*-----------------------------------------------------------------------------
	ncomp/intf/Logger.h
	This file define the interface of compile logs.
-----------------------------------------------------------------------------*/
#pragma once
#include "TypeDef.h"
#include <string>
#include <list>
#include <iosfwd>
#include <memory>

namespace nlang
{
	
//The basic abstract class for compiling log.
class NLANG_COMPILER_API CompileLogger
{
public:
	static const char* LOG_LEVEL_NAMES[CLL_COUNT];
public:
	CompileLogger(): m_nWarnings(0), m_nErrors(0), m_nFatals(0)
	{
	}

	virtual ~CompileLogger() = 0 {}

	inline size_t Fatals() const
	{
		return m_nFatals;
	}

	inline size_t Errors() const
	{
		return m_nErrors;
	}

	inline size_t Warnings() const
	{
		return m_nWarnings;
	}

	inline void Recount()
	{
		m_nWarnings = 0;
		m_nErrors = 0;
		m_nFatals = 0;
	}

	void Log(CompileLogLevel, const char* zFormat, ...);

	void Log(CompileLogLevel, const ISourceLocation*, 
		const char* zFormat, ...);

	void VLog(CompileLogLevel, const ISourceLocation*, const char* zFormat, 
		va_list& p);
protected:
	virtual void WriteLog(CompileLogLevel level, 
		const ISourceLocation* pLocation, const char* zMessage) = 0;
private:
	size_t m_nWarnings;
	size_t m_nErrors;
	size_t m_nFatals;
};

//The data structure contains the information in one log writing.
//A log item usually represents a text line in a log file.
class NLANG_COMPILER_API CompileLogItem
{
public:
	CompileLogItem(CompileLogLevel, ISourceLocation*, const char* zMessage);

	~CompileLogItem();

	inline CompileLogLevel Level() const
	{
		return m_Level;
	}

	inline ISourceLocation* Location() const
	{
		return m_pLocation;
	}

	inline const std::string& Message() const
	{
		return *m_upMessage;
	}

	std::string FullMessage() const;
private:
	CompileLogLevel m_Level;
	ISourceLocation* m_pLocation;
	std::unique_ptr<std::string> m_upMessage;
};

//A logger stored the log items in a list.
class NLANG_COMPILER_API ListCompileLogger: public CompileLogger
{
	typedef std::list<CompileLogItem*> ItemList;
	typedef ItemList::const_iterator const_iterator;
public:
	ListCompileLogger();

	virtual ~ListCompileLogger() override;

	//STL compatible methods
	inline const_iterator cbegin() const
	{
		return m_upItems->cbegin();
	}

	inline const_iterator cend() const
	{
		return m_upItems->cend();
	}

	inline size_t size() const
	{
		return m_upItems->size();
	}

	void clear();
protected:
	virtual void WriteLog(CompileLogLevel level, 
		const ISourceLocation* pLocation, const char* zMessage) override;
private:
	std::unique_ptr<ItemList> m_upItems;
};

//A logger stored the logs in an standard output stream.
class NLANG_COMPILER_API StreamCompileLogger: public CompileLogger
{
public:
	StreamCompileLogger(std::ostream&);

	inline std::ostream& Stream()
	{
		return m_Stream;
	}
protected:
	virtual void WriteLog(CompileLogLevel level, 
		const ISourceLocation* pLocation, const char* zMessage) override;
private:
	std::ostream& m_Stream;
};

} //namespace nlang
