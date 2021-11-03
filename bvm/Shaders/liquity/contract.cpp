////////////////////////
#include "../common.h"
#include "../Math.h"
#include "contract.h"

namespace Liquity {

template <uint8_t nTag>
struct EpochStorage
{
    static EpochKey get_Key(uint32_t iEpoch) {
        EpochKey k;
        k.m_Tag = nTag;
        k.m_iEpoch = iEpoch;
        return k;
    }

    static void Load(uint32_t iEpoch, ExchangePool::Epoch& e) {
        Env::LoadVar_T(get_Key(iEpoch), e);
    }

    static void Save(uint32_t iEpoch, const ExchangePool::Epoch& e) {
        Env::SaveVar_T(get_Key(iEpoch), e);
    }

    static void Del(uint32_t iEpoch) {
        Env::DelVar_T(get_Key(iEpoch));
    }
};

struct MyGlobal
    :public Global
{
    void Load()
    {
        auto key = Tags::s_State;
        Env::LoadVar_T(key, *this);
    }

    void Save()
    {
        auto key = Tags::s_State;
        Env::SaveVar_T(key, *this);
    }

    static void AdjustStrict(Amount& x, Amount val, uint8_t bAdd)
    {
        if (bAdd)
            Strict::Add(x, val);
        else
            Strict::Sub(x, val);
    }

    static void AdjustBank(const FlowPair& fp, const PubKey& pk)
    {
        Env::AddSig(pk);

        if (fp.Tok.m_Val || fp.Col.m_Val)
        {
            Balance::Key kub;
            _POD_(kub.m_Pk) = pk;

            Balance ub;
            if (!Env::LoadVar_T(kub, ub))
                _POD_(ub).SetZero();

            AdjustStrict(ub.m_Amounts.Tok, fp.Tok.m_Val, fp.Tok.m_Spend);
            AdjustStrict(ub.m_Amounts.Col, fp.Col.m_Val, fp.Col.m_Spend);

            if (ub.m_Amounts.Tok || ub.m_Amounts.Col)
                Env::SaveVar_T(kub, ub);
            else
                Env::DelVar_T(kub);
        }
    }

    static void AdjustTxFunds(const Flow& f, AssetID aid)
    {
        if (f.m_Val)
        {
            if (f.m_Spend)
                Env::FundsLock(aid, f.m_Val);
            else
                Env::FundsUnlock(aid, f.m_Val);
        }
    }

    void AdjustTxFunds(const Method::BaseTx& r) const
    {
        AdjustTxFunds(r.m_Flow.Tok, m_Aid);
        AdjustTxFunds(r.m_Flow.Col, 0);
    }

    void AdjustTxFundsAndEmission(const FlowPair& fpTotals, const Method::BaseTx& r) const
    {
        if (fpTotals.Tok.m_Val && !fpTotals.Tok.m_Spend)
            Env::Halt_if(!Env::AssetEmit(m_Aid, fpTotals.Tok.m_Val, 1));

        AdjustTxFunds(r);

        if (fpTotals.Tok.m_Val && fpTotals.Tok.m_Spend)
            Env::Halt_if(!Env::AssetEmit(m_Aid, fpTotals.Tok.m_Val, 0));
    }

    void AdjustTxBank(const FlowPair& fpLogic, const Method::BaseTx& r, const PubKey& pk)
    {
        FlowPair fpDelta = r.m_Flow;
        fpDelta.Tok -= fpLogic.Tok;
        fpDelta.Col -= fpLogic.Col;

        AdjustBank(fpDelta, pk);
    }

    void AdjustTotals(const FlowPair& fp)
    {
        AdjustStrict(m_Troves.m_Totals.Tok, fp.Tok.m_Val, !fp.Tok.m_Spend); // reverse direction!
        AdjustStrict(m_Troves.m_Totals.Col, fp.Col.m_Val, fp.Col.m_Spend);
    }

    void AdjustAll(const Method::BaseTx& r, const FlowPair& fpTotals, const FlowPair& fpLogic, const PubKey& pk)
    {
        AdjustTxFundsAndEmission(fpTotals, r);
        AdjustTxBank(fpLogic, r, pk);
        AdjustTotals(fpTotals);
    }

    void TrovePop(const Trove::Key& tk, Trove& t)
    {
        Env::Halt_if(!Env::LoadVar_T(tk, t));

        EpochStorage<Tags::s_Epoch_Redist> stor;
        m_RedistPool.Remove(t, stor);
    }

    bool TrovePush(const Trove::Key& tk, Trove& t)
    {
        m_RedistPool.Add(t);
        return Env::SaveVar_T(tk, t); // returns if existed already (i.e. overwriting)
    }

    void TroveModify(Trove::ID tid, const Pair* pNewVals, const PubKey* pPk, const Method::BaseTx& r)
    {
        bool bOpen = !!pPk;
        bool bClose = !pNewVals;

        FlowPair fpTotals, fpLogic;
        Trove t;
        Trove::Key tk;

        if (bOpen)
        {
            _POD_(t).SetZero();
            _POD_(t.m_pkOwner) = *pPk;
            tk.m_iTrove = ++m_Troves.m_iLastCreated;

            _POD_(fpTotals).SetZero();
        }
        else
        {
            tk.m_iTrove = tid;
            TrovePop(tk, t);

            fpTotals.Tok.m_Val = t.m_Amounts.Tok;
            fpTotals.Tok.m_Spend = 1;
            fpTotals.Col.m_Val = t.m_Amounts.Col;
            fpTotals.Col.m_Spend = 0;
        }

        if (!bClose)
        {
            fpTotals.Tok.Add(pNewVals->Tok, 0);
            fpTotals.Col.Add(pNewVals->Col, 1);
        }

        _POD_(fpLogic) = fpTotals;

        if (bOpen)
            fpLogic.Tok.Add(m_Settings.m_TroveLiquidationReserve, 1);

        Price price;
        Float trcr0;

        if (bClose)
            fpLogic.Tok.Add(m_Settings.m_TroveLiquidationReserve, 0);
        else
        {
            price = get_Price();
            trcr0 = m_Troves.get_Trcr();
        }

        if (fpTotals.Tok.m_Val && !fpTotals.Tok.m_Spend && !m_ProfitPool.IsEmpty())
        {
            UpdateBaseRate();

            Amount fee = m_kBaseRate * Float(fpTotals.Tok.m_Val);
            fee = std::min(fee, fpTotals.Tok.m_Val);

            m_ProfitPool.AddValue(fee, 0);
            fpLogic.Tok.Add(fee, 1);
        }

        AdjustAll(r, fpTotals, fpLogic, t.m_pkOwner); // will invoke AddSig

        if (bClose)
            Env::DelVar_T(tid);
        else
        {
            t.m_Amounts = *pNewVals;
            Env::Halt_if(t.m_Amounts.Tok < m_Settings.m_TroveMinDebt);

            // TODO: check icr, tcr, ensure tcr doesn't decrease if in recovery mode

            bool bExisted = TrovePush(tk, t);
            Env::Halt_if(bOpen && bExisted);
        }
    }


    //static void FundsAccountForUser(const FundsMove::Component& cLogic, const FundsMove::Component& cUser, AssetID aid)
    //{
    //    if (!cUser.m_Val)
    //        return;

    //    if (cUser.m_Spend)
    //        Env::FundsLock(aid, cUser.m_Val);
    //    else
    //        Env::FundsUnlock(aid, cUser.m_Val);

    //    cContract -= cUser;
    //}


    //void FinalyzeTx(const FundsMove& fmUser, const FundsMove& fmContract, const PubKey& pk) const
    //{
    //    auto fmDiff = fmContract;
    //    FundsAccountForUser(fmDiff.s, fmUser.s, m_Aid);
    //    FundsAccountForUser(fmDiff.b, fmUser.b, 0);

    //    if (fmDiff.s.m_Val || fmDiff.b.m_Val)
    //    {
    //        Balance::Key kub;
    //        _POD_(kub.m_Pk) = pk;

    //        Balance ub;
    //        if (!Env::LoadVar_T(kub, ub))
    //            _POD_(ub).SetZero();

    //        BalanceAdjustStrict(ub.m_Amounts.s, fmDiff.s);
    //        BalanceAdjustStrict(ub.m_Amounts.b, fmDiff.b);

    //        if (ub.m_Amounts.s || ub.m_Amounts.b)
    //            Env::SaveVar_T(kub, ub);
    //        else
    //            Env::DelVar_T(kub);
    //    }
    //}

    //void FinalyzeTxAndEmission(const FundsMove& fmUser, FundsMove& fmContract, Trove& t)
    //{
    //    const auto& s_ = fmContract.s;
    //    if (s_.m_Val && !s_.m_Spend)
    //        Env::Halt_if(!Env::AssetEmit(m_Aid, s_.m_Val, 1));

    //    FinalyzeTx(fmUser, fmContract, t.m_pkOwner);

    //    if (s_.m_Val && s_.m_Spend)
    //        Env::Halt_if(!Env::AssetEmit(m_Aid, s_.m_Val, 0));
    //}

    //void AdjustTroveAndTotals(const FundsMove& fmContract, Trove& t)
    //{
    //    auto b_ = fmContract.b;
    //    b_.m_Spend = !b_.m_Spend; // invert

    //    BalanceAdjustStrict(m_Troves.m_Totals.s, fmContract.s);
    //    BalanceAdjustStrict(m_Troves.m_Totals.b, b_);

    //    BalanceAdjustStrict(t.m_Amounts.s, fmContract.s);
    //    BalanceAdjustStrict(t.m_Amounts.b, b_);
    //}

    //Trove::ID TrovePop(Trove& t, Trove::ID iPrev)
    //{
    //    Trove::Key tk;
    //    if (iPrev)
    //    {
    //        Trove::Key tkPrev;
    //        tkPrev.m_iTrove = iPrev;
    //        Trove tPrev;
    //        Env::Halt_if(!Env::LoadVar_T(tkPrev, tPrev));

    //        tk.m_iTrove = tPrev.m_iRcrNext;
    //        Env::Halt_if(!Env::LoadVar_T(tk, t));

    //        tPrev.m_iRcrNext = t.m_iRcrNext;
    //        Env::SaveVar_T(tkPrev, tPrev); // TODO - we may omit this, if after manipulations t goes at the same position
    //       
    //    }
    //    else
    //    {
    //        tk.m_iTrove = m_Troves.m_iRcrLow;
    //        Env::Halt_if(!Env::LoadVar_T(tk, t));

    //        m_Troves.m_iRcrLow = t.m_iRcrNext;
    //    }

    //    EpochStorage<Tags::s_Epoch_Redist> stor;
    //    m_RedistPool.Remove(t, stor);

    //    return tk.m_iTrove;
    //}

    //void TrovePush(Trove& t, Trove::ID iTrove, MultiPrecision::Float rcr, Trove::ID iPrev)
    //{
    //    Trove::Key tk;

    //    if (iPrev)
    //    {
    //        Trove tPrev;
    //        tk.m_iTrove = iPrev;
    //        Env::Halt_if(!Env::LoadVar_T(tk, tPrev));

    //        tk.m_iTrove = tPrev.m_iRcrNext;
    //        tPrev.m_iRcrNext = iTrove;
    //        Env::SaveVar_T(tk, tPrev);

    //        Env::Halt_if(tPrev.get_Rcr() > rcr);
    //    }
    //    else
    //    {
    //        tk.m_iTrove = m_Troves.m_iRcrLow;
    //        m_Troves.m_iRcrLow = iTrove;
    //    }

    //    if (tk.m_iTrove)
    //    {
    //        Trove tNext;
    //        Env::Halt_if(!Env::LoadVar_T(tk, tNext));
    //        Env::Halt_if(rcr > tNext.get_Rcr());
    //    }

    //    t.m_iRcrNext = tk.m_iTrove;
    //    m_RedistPool.Add(t);

    //    tk.m_iTrove = iTrove;
    //    Env::SaveVar_T(tk, t);
    //}

    //void TrovePushValidate(Trove& t, Trove::ID iTrove, Trove::ID iPrev, const Global::Price* pPrice)
    //{
    //    MultiPrecision::Float rcr = t.get_Rcr();
    //    TrovePush(t, iTrove, rcr, iPrev);

    //    if (!pPrice)
    //        return; // forced trove update, no need to verify icr

    //    Env::Halt_if(t.m_Amounts.s < m_Settings.m_TroveMinTokens); // trove must have minimum tokens

    //    MultiPrecision::Float trcr = m_Troves.get_Trcr();
    //    if (pPrice->IsBelow150(trcr))
    //        // recovery mode
    //        Env::Halt_if(pPrice->IsBelow150(rcr));
    //    else
    //        Env::Halt_if(pPrice->IsBelow110(rcr));
    //}

    Global::Price get_Price()
    {
        Method::OracleGet args;
        Env::CallFar_T(m_Settings.m_cidOracle, args, 0);

        Global::Price ret;
        ret.m_Value = args.m_Val;
        return ret;
    }
};

struct MyGlobal_Load
    :public MyGlobal
{
    MyGlobal_Load() {
        Load();
    }
};

struct MyGlobal_LoadSave
    :public MyGlobal_Load
{
    ~MyGlobal_LoadSave()
    {
#ifdef HOST_BUILD
        if (std::uncaught_exceptions())
            return;
#endif // HOST_BUILD
        Save();
    }
};

BEAM_EXPORT void Ctor(const Method::Create& r)
{
    MyGlobal g;
    _POD_(g).SetZero();

    _POD_(g.m_Settings) = r.m_Settings;
    g.m_StabPool.Init();
    g.m_RedistPool.Init();

    static const char szMeta[] = "STD:SCH_VER=1;N=Liquity Token;SN=Liqt;UN=LIQT;NTHUN=GROTHL";
    g.m_Aid = Env::AssetCreate(szMeta, sizeof(szMeta) - 1);
    Env::Halt_if(!g.m_Aid);

    Env::Halt_if(!Env::RefAdd(g.m_Settings.m_cidOracle));

    g.Save();
}

BEAM_EXPORT void Dtor(void*)
{
}

BEAM_EXPORT void Method_2(void*)
{
    // called on upgrade
}

BEAM_EXPORT void Method_3(const Method::TroveOpen& r)
{
    MyGlobal_LoadSave g;
    g.TroveModify(0, &r.m_Amounts, &r.m_pkUser, r);
}

BEAM_EXPORT void Method_4(const Method::TroveClose& r)
{
    MyGlobal_LoadSave g;
    g.TroveModify(r.m_iTrove, nullptr, nullptr, r);
}

BEAM_EXPORT void Method_5(Method::TroveModify& r)
{
    MyGlobal_LoadSave g;
    g.TroveModify(r.m_iTrove, &r.m_Amounts, nullptr, r);
}

BEAM_EXPORT void Method_6(const Method::FundsAccess& r)
{
    MyGlobal_Load g;
    g.AdjustTxFunds(r);
    g.AdjustBank(r.m_Flow, r.m_pkUser); // will invoke AddSig
}

BEAM_EXPORT void Method_7(Method::UpdStabPool& r)
{
    MyGlobal_LoadSave g;

    StabPoolEntry::Key spk;
    _POD_(spk.m_pkUser) = r.m_pkUser;

    FlowPair fpLogic;
    _POD_(fpLogic).SetZero();

    StabPoolEntry spe;
    if (!Env::LoadVar_T(spk, spe))
        _POD_(spe).SetZero();
    else
    {
        EpochStorage<Tags::s_Epoch_Stable> stor;

        HomogenousPool::Pair out;
        g.m_StabPool.UserDel(spe.m_User, out, stor);

        fpLogic.Tok.m_Val = out.s;
        fpLogic.Col.m_Val = out.b;
    }

    if (r.m_NewAmount)
    {
        g.m_StabPool.UserAdd(spe.m_User, r.m_NewAmount);
        Env::SaveVar_T(spk, spe);

        fpLogic.Tok.Add(r.m_NewAmount, 1);
    }
    else
        Env::DelVar_T(spk);

    g.AdjustTxFunds(r);
    g.AdjustTxBank(fpLogic, r, r.m_pkUser);
}

BEAM_EXPORT void Method_8(Method::EnforceLiquidatation& r)
{
    MyGlobal_LoadSave g;

    FlowPair fpTotals, fpLogic;
    _POD_(fpTotals).SetZero();
    _POD_(fpLogic).SetZero();

    auto price = g.get_Price();

    auto pId = reinterpret_cast<const Trove::ID*>(&r + 1);
    Trove::Key tk;

    bool bStab = false, bRedist = false;

    for (uint32_t i = 0; i < r.m_Count; i++)
    {
        tk.m_iTrove = pId[i];
        Trove t;
        g.TrovePop(tk, t);

        auto icr = price.m_Value * t.get_Rcr();

        // TODO: deduce mode, decide if liquidation is ok
        Env::Halt_if(icr >= Global::Price::get_k110());

        fpTotals.Tok.Add(t.m_Amounts.Tok, 1); // all debt would be burned, unless redist pool is used
        fpTotals.Col.Add(t.m_Amounts.Col, 0);

        assert(t.m_Amounts.Tok >= g.m_Settings.m_TroveLiquidationReserve);

        Amount valBurn = g.get_LiquidationRewardReduce(icr);
        assert(valBurn < g.m_Settings.m_TroveLiquidationReserve);
        Amount valReward = g.m_Settings.m_TroveLiquidationReserve - valBurn;

        fpLogic.Tok.Add(valReward, 0); // goes to the liquidator
        t.m_Amounts.Tok -= valBurn; // goes as a 'discount' for the stab pool

        if (icr > Global::Price::get_k100())
        {
            if (g.m_StabPool.LiquidatePartial(t))
                bStab = true;
        }

        if (t.m_Amounts.Tok || t.m_Amounts.Col)
        {
            bRedist = true;
            Env::Halt_if(!g.m_RedistPool.Liquidate(t));

            fpTotals.Tok.Add(t.m_Amounts.Tok, 0);
            fpTotals.Col.Add(t.m_Amounts.Col, 1);
        }
    }

    if (bStab)
    {
        EpochStorage<Tags::s_Epoch_Stable> stor;
        g.m_StabPool.OnPostTrade(stor);
    }

    if (bRedist)
    {
        EpochStorage<Tags::s_Epoch_Redist> stor;
        g.m_RedistPool.OnPostTrade(stor);
    }

    g.AdjustAll(r, fpTotals, fpLogic, r.m_pkUser);
}

BEAM_EXPORT void Method_9(Method::UpdProfitPool& r)
{
    MyGlobal_LoadSave g;

    ProfitPoolEntry::Key pk;
    _POD_(pk.m_pkUser) = r.m_pkUser;

    FlowPair fpLogic;
    _POD_(fpLogic).SetZero();

    ProfitPoolEntry pe;
    if (!Env::LoadVar_T(pk, pe))
        _POD_(pe).SetZero();
    else
    {
        Amount pOut[2];
        g.m_ProfitPool.Remove(pOut, pe.m_User);

        fpLogic.Tok.m_Val = pOut[0];
        fpLogic.Col.m_Val = pOut[1];
    }

    if (r.m_NewAmount > pe.m_User.m_Weight)
        Env::FundsLock(g.m_Settings.m_AidProfit, r.m_NewAmount - pe.m_User.m_Weight);

    if (pe.m_User.m_Weight > r.m_NewAmount)
        Env::FundsUnlock(g.m_Settings.m_AidProfit, pe.m_User.m_Weight - r.m_NewAmount);

    if (r.m_NewAmount)
    {
        pe.m_User.m_Weight = r.m_NewAmount;
        g.m_ProfitPool.Add(pe.m_User);

        Env::SaveVar_T(pk, pe);
    }
    else
        Env::DelVar_T(pk);

    g.AdjustTxFunds(r);
    g.AdjustTxBank(fpLogic, r, r.m_pkUser);
}

} // namespace Liquity