////////////////////////
#include "../common.h"
#include "contract.h"
#include "../dao-vault/contract.h"
#include "../vault_anon/contract.h"

namespace NameService {

BEAM_EXPORT void Ctor(const Method::Create& r)
{
    Env::Halt_if(!Env::RefAdd(r.m_Settings.m_cidDaoVault));
    Env::Halt_if(!Env::RefAdd(r.m_Settings.m_cidVault));
    Env::SaveVar_T((uint8_t) Tags::s_Settings, r.m_Settings);
}

BEAM_EXPORT void Dtor(void*)
{
}

void SendProfit(Amount val);

struct MyDomain
    :public Domain
{
    Domain::KeyMax m_Key;
    uint32_t m_KeyLen;

    MyDomain(const uint8_t& nNameLen)
    {
        uint32_t delta = nNameLen - s_MinLen;
        Env::Halt_if(delta > s_MaxLen - s_MinLen);

        Env::Memcpy(m_Key.m_sz, &nNameLen + 1, nNameLen);
        m_KeyLen = nNameLen + 1;
    }

    bool Load()
    {
        return Env::LoadVar(&m_Key, m_KeyLen, &Cast::Down<Domain>(*this), sizeof(Domain), KeyTag::Internal) == sizeof(Domain);
    }

    bool Save() const
    {
        return Env::SaveVar(&m_Key, m_KeyLen, &Cast::Down<Domain>(*this), sizeof(Domain), KeyTag::Internal) == sizeof(Domain);
    }

    void Extend(uint8_t nPeriods, uint32_t nNameLen)
    {
        if (!nPeriods)
            nPeriods = 1;

        Height h = Env::get_Height();
        if (m_hExpire < h)
            m_hExpire = h;

        m_hExpire += s_PeriodValidity * nPeriods; // can't overflow
        Env::Halt_if(m_hExpire > h + s_PeriodValidityMax);

        Amount val = Domain::get_Price(nNameLen);
        SendProfit(val * nPeriods);
    }
};

struct MySettings :public Settings
{
    MySettings()
    {
        Env::LoadVar_T((uint8_t) Tags::s_Settings, *this);
    }
};

void SendProfit(Amount val)
{
    DaoVault::Method::Deposit arg;
    arg.m_Aid = 0;
    arg.m_Amount = val;

    MySettings stg;
    Env::CallFar_T(stg.m_cidDaoVault, arg);
}

BEAM_EXPORT void Method_2(const Method::Register& r)
{
    Height h = Env::get_Height();
    MyDomain d(r.m_NameLen);
    if (d.Load())
        Env::Halt_if(!d.IsExpired(h));
    else
    {
        // check name
        for (uint32_t i = 0; i < r.m_NameLen; i++)
            Env::Halt_if(!d.IsValidChar(d.m_Key.m_sz[i]));
    }

    _POD_(Cast::Down<Domain>(d)).SetZero();
    _POD_(d.m_pkOwner) = r.m_pkOwner;

    d.Extend(r.m_Periods, r.m_NameLen);

    d.Save();
}

BEAM_EXPORT void Method_3(const Method::SetOwner& r)
{
    MyDomain d(r.m_NameLen);
    Env::Halt_if(!d.Load() || d.IsExpired(Env::get_Height()));

    Env::AddSig(d.m_pkOwner);
    _POD_(d.m_pkOwner) = r.m_pkNewOwner;


    d.Save();
}

BEAM_EXPORT void Method_4(const Method::Extend& r)
{
    MyDomain d(r.m_NameLen);
    Env::Halt_if(!d.Load());

    d.Extend(r.m_Periods, r.m_NameLen);

    d.Save();
}

BEAM_EXPORT void Method_5(const Method::SetPrice& r)
{
    MyDomain d(r.m_NameLen);
    Env::Halt_if(!d.Load() || d.IsExpired(Env::get_Height()));

    Env::AddSig(d.m_pkOwner);
    d.m_Price = r.m_Price;

    d.Save();
}

BEAM_EXPORT void Method_6(const Method::Buy& r)
{
    MyDomain d(r.m_NameLen);
    Env::Halt_if(!d.Load() || d.IsExpired(Env::get_Height()));

    Env::Halt_if(!d.m_Price.m_Amount); // should be for sale

    {
        VaultAnon::Method::Deposit arg;
        arg.m_SizeCustom = 0;
        arg.m_Amount = d.m_Price.m_Amount;
        arg.m_Key.m_Aid = d.m_Price.m_Aid;
        _POD_(arg.m_Key.m_pkOwner) = d.m_pkOwner;

        MySettings stg;
        Env::CallFar_T(stg.m_cidVault, arg);
    }

    _POD_(d.m_Price).SetZero();
    _POD_(d.m_pkOwner) = r.m_pkNewOwner;

    d.Save();
}

} // namespace NameService
