/*
 * Copyright 2015, LAAS-CNRS
 * Author: Andrea Del Prete
 */

#include <robust-equilibrium-lib/static_equilibrium.hh>
#include <robust-equilibrium-lib/logger.hh>
#include <robust-equilibrium-lib/stop-watch.hh>
#include <iostream>
#include <vector>
#include <ctime>

using namespace std;

namespace robust_equilibrium
{

bool StaticEquilibrium::m_is_cdd_initialized = false;

StaticEquilibrium::StaticEquilibrium(string name, double mass, unsigned int generatorsPerContact,
                                     SolverLP solver_type, bool useWarmStart)
{
  if(!m_is_cdd_initialized)
  {
    init_cdd_library();
    m_is_cdd_initialized = true;
  }

  if(generatorsPerContact<3)
  {
    SEND_WARNING_MSG("Algorithm cannot work with less than 3 generators per contact!");
    generatorsPerContact = 3;
  }

  m_name = name;
  m_solver_type = solver_type;
  m_solver = Solver_LP_abstract::getNewSolver(solver_type);
  m_solver->setUseWarmStart(useWarmStart);

  m_generatorsPerContact = generatorsPerContact;
  m_mass = mass;
  m_gravity.setZero();
  m_gravity(2) = -9.81;

  m_d.setZero();
  m_d.head<3>() = m_mass*m_gravity;
  m_D.setZero();
  m_D.block<3,3>(3,0) = crossMatrix(-m_mass*m_gravity);
}

bool StaticEquilibrium::setNewContacts(Cref_matrixX3 contactPoints, Cref_matrixX3 contactNormals,
                                       double frictionCoefficient, StaticEquilibriumAlgorithm alg)
{
  assert(contactPoints.rows()==contactNormals.rows());

  if(alg==STATIC_EQUILIBRIUM_ALGORITHM_IP)
  {
    SEND_ERROR_MSG("Algorithm IP not implemented yet");
    return false;
  }
  if(alg==STATIC_EQUILIBRIUM_ALGORITHM_DIP)
  {
    SEND_ERROR_MSG("Algorithm DIP not implemented yet");
    return false;
  }

  m_algorithm = alg;

  long int c = contactPoints.rows();
  unsigned int &cg = m_generatorsPerContact;
  double theta, delta_theta=2*M_PI/cg;
  // Tangent directions
  Vector3 T1, T2;
  // Matrix mapping a 3d contact force to gravito-inertial wrench (6 X 3)
  Matrix63 A;
  A.topRows<3>() = -Matrix3::Identity();
  // Lists of contact generators (3 X generatorsPerContact)
  Matrix3X G(3, cg);
  m_G_centr.resize(6,c*cg);

  for(long int i=0; i<c; i++)
  {
    // check that contact normals have norm 1
    if(fabs(contactNormals.row(i).norm()-1.0)>1e-6)
    {
      SEND_ERROR_MSG("Contact normals should have norm 1, this has norm %f"+toString(contactNormals.row(i).norm()));
      return false;
    }
    // compute tangent directions
    T1 = contactNormals.row(i).cross(Vector3::UnitY());
    if(T1.norm()<1e-5)
      T1 = contactNormals.row(i).cross(Vector3::UnitX());
    T2 = contactNormals.row(i).transpose().cross(T1);
    T1.normalize();
    T2.normalize();

    // compute matrix mapping contact forces to gravito-inertial wrench
    A.bottomRows<3>() = crossMatrix(-1.0*contactPoints.row(i).transpose());

    // compute generators
    theta = 0.0;
    for(int j=0; j<cg; j++)
    {
      G.col(j) = frictionCoefficient*sin(theta)*T1
                + frictionCoefficient*cos(theta)*T2
                + contactNormals.row(i).transpose();
      G.col(j).normalize();
//      SEND_DEBUG_MSG("Contact "+toString(i)+" generator "+toString(j)+" = "+toString(G.col(j).transpose()));
      theta += delta_theta;
    }

    // project generators in 6d centroidal space
    m_G_centr.block(0,cg*i,6,cg) = A * G;
  }

  // Compute the coefficient to convert b0 to e_max
  Vector3 f0 = Vector3::Zero();
  for(int j=0; j<cg; j++)
    f0 += G.col(j); // sum of the contact generators
  // Compute the distance between the friction cone boundaries and
  // the sum of the contact generators, which is e_max when b0=1.
  // When b0!=1 we just multiply b0 times this value.
  // This value depends only on the number of generators and the friction coefficient
  m_b0_to_emax_coefficient = (f0.cross(G.col(0))).norm();

  if(m_algorithm==STATIC_EQUILIBRIUM_ALGORITHM_PP)
  {
    if(!computePolytopeProjection(m_G_centr))
      return false;
    m_HD = m_H * m_D;
    m_Hd = m_H * m_d;
  }

  return true;
}


LP_status StaticEquilibrium::computeEquilibriumRobustness(Cref_vector3 com, double &robustness)
{
  const long m = m_G_centr.cols(); // number of gravito-inertial wrench generators
  if(m==0)
    return LP_STATUS_INFEASIBLE;

  if(m_algorithm==STATIC_EQUILIBRIUM_ALGORITHM_LP)
  {
    /* Compute the robustness measure of the equilibrium of a specified CoM position
     * by solving the following LP:
          find          b, b0
          minimize      -b0
          subject to    D c + d <= G b    <= D c + d
                        0       <= b - b0 <= Inf
        where
          b         are the coefficient of the contact force generators (f = V b)
          b0        is the robustness measure
          c         is the CoM position
          G         is the matrix whose columns are the gravito-inertial wrench generators
    */
    VectorX b_b0(m+1);
    VectorX c = VectorX::Zero(m+1);
    c(m) = -1.0;
    VectorX lb = -VectorX::Ones(m+1)*1e5;
    VectorX ub = VectorX::Ones(m+1)*1e10;
    VectorX Alb = VectorX::Zero(6+m);
    VectorX Aub = VectorX::Ones(6+m)*1e100;
    MatrixXX A = MatrixXX::Zero(6+m, m+1);
    Alb.head<6>() = m_D * com + m_d;
    Aub.head<6>() = Alb.head<6>();
    A.topLeftCorner(6,m)      = m_G_centr;
    A.bottomLeftCorner(m,m)   = MatrixXX::Identity(m,m);
    A.bottomRightCorner(m,1)  = -VectorX::Ones(m);

    LP_status lpStatus = m_solver->solve(c, lb, ub, A, Alb, Aub, b_b0);
    if(lpStatus==LP_STATUS_OPTIMAL)
    {
      robustness = convert_b0_to_emax(-1.0*m_solver->getObjectiveValue());
      return lpStatus;
    }

    SEND_DEBUG_MSG("Primal LP problem could not be solved: "+toString(lpStatus));
    return lpStatus;
  }

  if(m_algorithm==STATIC_EQUILIBRIUM_ALGORITHM_LP2)
  {
    /* Compute the robustness measure of the equilibrium of a specified CoM position
     * by solving the following LP:
            find          b, b0
            minimize      -b0
            subject to    D c + d <= G (b + 1*b0) <= D c + d
                          0       <= b            <= Inf
          where
            b         are the (delta) coefficient of the contact force generators (f = G (b+b0))
            b0        is the robustness measure
            c         is the CoM position
            G         is the matrix whose columns are the gravito-inertial wrench generators
      */
    VectorX b_b0(m+1);
    VectorX c = VectorX::Zero(m+1);
    c(m) = -1.0;
    VectorX lb = VectorX::Zero(m+1);
    lb(m) = -1e10;
    VectorX ub = VectorX::Ones(m+1)*1e10;
    MatrixXX A = MatrixXX::Zero(6, m+1);
    Vector6 Alb = m_D * com + m_d;
    Vector6 Aub = Alb;
    A.leftCols(m)  = m_G_centr;
    A.rightCols(1) = m_G_centr * VectorX::Ones(m);

    LP_status lpStatus_primal = m_solver->solve(c, lb, ub, A, Alb, Aub, b_b0);
    if(lpStatus_primal==LP_STATUS_OPTIMAL)
    {
      robustness = convert_b0_to_emax(-1.0*m_solver->getObjectiveValue());
      return lpStatus_primal;
    }

    SEND_DEBUG_MSG("Primal LP problem could not be solved: "+toString(lpStatus_primal));
    return lpStatus_primal;
  }

  if(m_algorithm==STATIC_EQUILIBRIUM_ALGORITHM_DLP)
  {
    /*Compute the robustness measure of the equilibrium of a specified CoM position
      by solving the following dual LP:
        find          v
        minimize      (d+D*com)' v
        subject to    G' v >= 0
                      1' G' v = 1
      where
        -(d+D c)' v   is the robustness measure
        c             is the CoM position
        G             is the matrix whose columns are the gravito-inertial wrench generators
     */
    Vector6 v;
    Vector6 c = m_D*com + m_d;
    Vector6 lb = Vector6::Ones()*-1e100;
    Vector6 ub = Vector6::Ones()*1e100;
    VectorX Alb = VectorX::Zero(m+1);
    Alb(m) = 1.0;
    VectorX Aub = VectorX::Ones(m+1)*1e100;
    Aub(m) = 1.0;
    MatrixX6 A(m+1,6);
    A.topRows(m) = m_G_centr.transpose();
    A.bottomRows<1>() = (m_G_centr*VectorX::Ones(m)).transpose();

    LP_status lpStatus_dual = m_solver->solve(c, lb, ub, A, Alb, Aub, v);
    if(lpStatus_dual==LP_STATUS_OPTIMAL)
    {
      robustness = convert_b0_to_emax(m_solver->getObjectiveValue());
      return lpStatus_dual;
    }
    SEND_DEBUG_MSG("Dual LP problem for com position "+toString(com.transpose())+" could not be solved: "+toString(lpStatus_dual));

    // switch UNFEASIBLE and UNBOUNDED flags because we are solving dual problem
    if(lpStatus_dual==LP_STATUS_INFEASIBLE)
      lpStatus_dual = LP_STATUS_UNBOUNDED;
    else if(lpStatus_dual==LP_STATUS_UNBOUNDED)
      lpStatus_dual = LP_STATUS_INFEASIBLE;

    return lpStatus_dual;
  }

  SEND_ERROR_MSG("checkRobustEquilibrium is not implemented for the specified algorithm");
  return LP_STATUS_ERROR;
}

LP_status StaticEquilibrium::checkRobustEquilibrium(Cref_vector3 com, bool &equilibrium, double e_max)
{
  if(m_G_centr.cols()==0)
  {
    equilibrium=false;
    return LP_STATUS_OPTIMAL;
  }
  if(e_max!=0.0)
  {
    SEND_ERROR_MSG("checkRobustEquilibrium with e_max!=0 not implemented yet");
    return LP_STATUS_ERROR;
  }
  if(m_algorithm!=STATIC_EQUILIBRIUM_ALGORITHM_PP)
  {
    SEND_ERROR_MSG("checkRobustEquilibrium is only implemented for the PP algorithm");
    return LP_STATUS_ERROR;
  }

  VectorX res = m_HD * com + m_Hd;
  for(long i=0; i<res.size(); i++)
    if(res(i)>0.0)
    {
      equilibrium = false;
      return LP_STATUS_OPTIMAL;
    }

  equilibrium = true;
  return LP_STATUS_OPTIMAL;
}

LP_status StaticEquilibrium::findExtremumOverLine(Cref_vector3 a, Cref_vector3 a0, double e_max, Ref_vector3 com)
{
  const long m = m_G_centr.cols(); // number of gravito-inertial wrench generators
  if(m_G_centr.cols()==0)
    return LP_STATUS_INFEASIBLE;

  double b0 = convert_emax_to_b0(e_max);

  if(m_algorithm==STATIC_EQUILIBRIUM_ALGORITHM_LP)
  {
    /* Compute the extremum CoM position over the line a*p + a0 that is in robust equilibrium
     * by solving the following LP:
          find          b, p
          minimize      -p
          subject to    D (a p + a0) + d <= G (b + b0) <= D (a p + a0) + d
                        0       <= b <= Inf
        where
          b0+b      are the coefficient of the contact force generators (f = G (b0+b))
          b0        is the robustness measure
          p         is the line parameter
          G         is the matrix whose columns are the gravito-inertial wrench generators
    */
    VectorX b_p(m+1);
    VectorX c = VectorX::Zero(m+1);
    c(m) = -1.0;
    VectorX lb = -VectorX::Zero(m+1);
    lb(m) = -1e5;
    VectorX ub = VectorX::Ones(m+1)*1e10;
    Vector6 Alb = m_D*a0 + m_d - m_G_centr*VectorX::Ones(m)*b0;
    Vector6 Aub = Alb;
    Matrix6X A = Matrix6X::Zero(6, m+1);
    A.leftCols(m)     = m_G_centr;
    A.rightCols(1)    = -m_D*a;

    LP_status lpStatus_primal = m_solver->solve(c, lb, ub, A, Alb, Aub, b_p);
    if(lpStatus_primal==LP_STATUS_OPTIMAL)
    {
      com = a0 + a*b_p(m);

//#define WRITE_LPS_TO_FILE
#ifdef WRITE_LPS_TO_FILE
      string date_time = getDateAndTimeAsString();
      string filename = "LP_findExtremumOverLine"+date_time+".dat";
      if(!m_solver->writeLpToFile(filename.c_str(), c, lb, ub, A, Alb, Aub))
        SEND_ERROR_MSG("Error while writing LP to file "+filename);
      filename = "LP_findExtremumOverLine"+date_time+"_solution.dat";
      if(!writeMatrixToFile(filename.c_str(), b_p))
        SEND_ERROR_MSG("Error while writing LP solution to file "+filename);
#endif

      return lpStatus_primal;
    }

    com = a0;
    SEND_DEBUG_MSG("Primal LP problem could not be solved suggesting that no equilibrium position with robustness "+
                     toString(e_max)+" exists over the line starting from "+toString(a0.transpose())+
                     " in direction "+toString(a.transpose())+", solver error code: "+toString(lpStatus_primal));
    return lpStatus_primal;
  }

  if(m_algorithm==STATIC_EQUILIBRIUM_ALGORITHM_DLP)
  {
    /* Compute the extremum CoM position over the line a*x + a0 that is in robust equilibrium
     * by solving the following dual LP:
          find          v
          minimize      (D a0 + d -G b0)' v
          subject to    0  <= G' v    <= Inf
                        -1 <= a' D' v <= -1
        where
          G         is the matrix whose columns are the gravito-inertial wrench generators
    */
    Vector6 v;
    Vector6 c = m_D*a0 + m_d - m_G_centr*VectorX::Ones(m)*b0;
    Vector6 lb = Vector6::Ones()*-1e10;
    Vector6 ub = Vector6::Ones()*1e10;
    VectorX Alb = VectorX::Zero(m+1);
    Alb(m) = -1.0;
    VectorX Aub = VectorX::Ones(m+1)*1e10;
    Aub(m) = -1.0;
    MatrixX6 A(m+1,6);
    A.topRows(m) = m_G_centr.transpose();
    A.bottomRows<1>() = (m_D*a).transpose();

    LP_status lpStatus_dual = m_solver->solve(c, lb, ub, A, Alb, Aub, v);
    if(lpStatus_dual==LP_STATUS_OPTIMAL)
    {
      double p = m_solver->getObjectiveValue();
      com = a0 + a*p;

#ifdef WRITE_LPS_TO_FILE
      string date_time = getDateAndTimeAsString();
      string filename = "DLP_findExtremumOverLine"+date_time+".dat";
      if(!m_solver->writeLpToFile(filename.c_str(), c, lb, ub, A, Alb, Aub))
        SEND_ERROR_MSG("Error while writing LP to file "+filename);
      filename = "DLP_findExtremumOverLine"+date_time+"_solution.dat";
      if(!writeMatrixToFile(filename.c_str(), v))
        SEND_ERROR_MSG("Error while writing LP solution to file "+filename);
#endif

      // since QP oases cannot detect unboundedness we check here whether the objective value is a large negative value
      if(m_solver_type==SOLVER_LP_QPOASES && p<-1e7)
      {
        SEND_DEBUG_MSG("Dual LP problem with robustness "+toString(e_max)+
                       " over the line starting from "+toString(a0.transpose())+
                       " in direction "+toString(a.transpose())+" has large negative objective value: "+toString(p)+
                       " suggesting it is probably unbounded.");
        lpStatus_dual = LP_STATUS_UNBOUNDED;
      }

      return lpStatus_dual;
    }

    com = a0;
    SEND_DEBUG_MSG("Dual LP problem could not be solved suggesting that no equilibrium position with robustness "+
                     toString(e_max)+" exists over the line starting from "+toString(a0.transpose())+
                     " in direction "+toString(a.transpose())+", solver error code: "+toString(lpStatus_dual));

    // switch UNFEASIBLE and UNBOUNDED flags because we are solving dual problem
    if(lpStatus_dual==LP_STATUS_INFEASIBLE)
      lpStatus_dual = LP_STATUS_UNBOUNDED;
    else if(lpStatus_dual==LP_STATUS_UNBOUNDED)
      lpStatus_dual = LP_STATUS_INFEASIBLE;

    return lpStatus_dual;
  }

  SEND_ERROR_MSG("findExtremumOverLine is not implemented for the specified algorithm");
  return LP_STATUS_ERROR;
}

LP_status StaticEquilibrium::findExtremumInDirection(Cref_vector3 direction, Ref_vector3 com, double e_max)
{
  if(m_G_centr.cols()==0)
    return LP_STATUS_INFEASIBLE;
  SEND_ERROR_MSG("findExtremumInDirection not implemented yet");
  return LP_STATUS_ERROR;
}

bool StaticEquilibrium::computePolytopeProjection(Cref_matrix6X v)
{
//  getProfiler().start("eigen_to_cdd");
  dd_MatrixPtr V = cone_span_eigen_to_cdd(v.transpose());
//  getProfiler().stop("eigen_to_cdd");

  dd_ErrorType error = dd_NoError;

//  getProfiler().start("dd_DDMatrix2Poly");
  dd_PolyhedraPtr H_= dd_DDMatrix2Poly(V, &error);
//  getProfiler().stop("dd_DDMatrix2Poly");

  if(error != dd_NoError)
  {
    SEND_ERROR_MSG("numerical instability in cddlib. ill formed polytope");
    return false;
  }

//  getProfiler().start("cdd to eigen");
  dd_MatrixPtr b_A = dd_CopyInequalities(H_);
  // get equalities and add them as complementary inequality constraints
  std::vector<long> eq_rows;
  for(long elem=1;elem<=(long)(b_A->linset[0]);++elem)
  {
    if (set_member(elem,b_A->linset))
      eq_rows.push_back(elem);
  }
  int rowsize = (int)b_A->rowsize;
  m_H.resize(rowsize + eq_rows.size(), (int)b_A->colsize-1);
  m_h.resize(rowsize + eq_rows.size());
  for(int i=0; i < rowsize; ++i)
  {
    m_h(i) = (value_type)(*(b_A->matrix[i][0]));
    for(int j=1; j < b_A->colsize; ++j)
      m_H(i, j-1) = -(value_type)(*(b_A->matrix[i][j]));
  }
  int i = 0;
  for(std::vector<long int>::const_iterator cit = eq_rows.begin(); cit != eq_rows.end(); ++cit, ++i)
  {
    m_h(rowsize + i) = -m_h((int)(*cit));
    m_H(rowsize + i) = -m_H((int)(*cit));
  }
//  getProfiler().stop("cdd to eigen");

  return true;
}

double StaticEquilibrium::convert_b0_to_emax(double b0)
{
  return (b0*m_b0_to_emax_coefficient);
}

double StaticEquilibrium::convert_emax_to_b0(double emax)
{
  return (emax/m_b0_to_emax_coefficient);
}

} // end namespace robust_equilibrium
